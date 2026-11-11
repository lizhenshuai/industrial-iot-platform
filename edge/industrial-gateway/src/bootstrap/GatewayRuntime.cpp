#include "GatewayRuntime.hpp"
#include "industrial_gateway/common/RuntimeFailure.hpp"
#include "RegisterBuiltInConnectors.hpp"
#include "../application/agent/GatewayAgent.hpp"
#include "../application/config/ConfigManager.hpp"
#include "../application/config/ConnectorConfigApplier.hpp"
#include "../application/connector/ConnectorFactoryRegistry.hpp"
#include "../application/connector/ConnectorManager.hpp"
#include "../application/ingestion/StorageWorker.hpp"
#include "../application/publishing/MqttPublisherWorker.hpp"
#include "../infrastructure/config/ConfigValidator.hpp"
#include "../infrastructure/concurrency/BoundedDataQueue.hpp"
#include "../infrastructure/concurrency/ConfigAckQueue.hpp"
#include "../infrastructure/concurrency/ConfigCommandQueue.hpp"
#include "../infrastructure/mqtt/MosquittoMqttClient.hpp"
#include "../infrastructure/storage/sqlite/SQLiteConfigStorage.hpp"
#include "../infrastructure/storage/sqlite/SQLiteMessageStorage.hpp"

#include <atomic>
#include <exception>
#include <string_view>
#include <thread>

namespace industrial_gateway::bootstrap
{
class GatewayRuntime::Impl
{
public:
    Impl(const config::GatewayConfig& config, common::ILogger& logger)
        : config_(config), logger_(logger), ingestion_storage_(config.storage),
          publishing_storage_(config.storage), config_storage_(config.storage),
          mqtt_(config.mqtt, logger), connectors_(registry_, data_queue_, logger),
          applier_(connectors_), configs_(commands_, config_storage_, applier_, acks_, logger),
          agent_(mqtt_, commands_, logger, config.gateway_id),
          ingestion_(data_queue_, ingestion_storage_, logger),
          publisher_(publishing_storage_, mqtt_, logger, config.gateway_id, &acks_) {}

    ~Impl() { stop(); }

    bool start(std::string& error_message)
    {
        error_message.clear();
        if (started_ && isRunning()) return true;
        if (attempted_ || stopped_)
        {
            error_message = "GatewayRuntime 已启动过或停止，请创建新实例";
            return false;
        }
        attempted_ = true;
        try
        {
            if (!infrastructure::config::ConfigValidator{}.validateGatewayConfig(config_, error_message))
                return failStart();
            // 动态模式只有 SQLite 恢复和 MQTT 快照这一条配置来源，避免静态配置
            // 与配置控制线程同时管理同一组连接。静态工厂接口仍供独立调用方使用。
            if (!config_.connections.empty())
            {
                error_message = "动态采集入口要求 gateway.yaml 中 connections 为 []；请通过 MQTT 下发采集配置";
                return failStart();
            }
            if (!registerBuiltInConnectors(registry_, error_message)) return failStart();

            // 消费者先启动，之后配置恢复创建的采集线程才有数据出口。
            storage_running_.store(true);
            storage_thread_ = std::thread([this]()
            {
                try { ingestion_.run(); }
                catch (const std::exception& error)
                {
                    storage_failure_.record("遥测存储线程异常退出", error.what());
                    log(common::LogLevel::Critical, "遥测存储线程异常退出");
                    log(common::LogLevel::Critical, error.what());
                }
                catch (...)
                {
                    storage_failure_.record("遥测存储线程发生未知异常");
                    log(common::LogLevel::Critical, "遥测存储线程发生未知异常");
                }
                if (!shutdown_requested_.load())
                    storage_failure_.record("遥测存储线程未经停止请求退出，内存中未落库数据不保证保留");
                storage_running_.store(false);
                // 消费者退出后关闭队列，避免其他生产者永久卡在满队列上。
                // 正常运行时不会走到这里；全局退出时重复 close 是安全的。
                data_queue_.close();
            });

            if (config_.mqtt.enabled)
            {
                // 先登记订阅，避免首次连接后遗漏 retained 配置。
                if (!agent_.start(error_message) || !mqtt_.start(error_message) ||
                    !publisher_.start(error_message)) return failStart();
            }
            // 每次进程启动先恢复 SQLite，随后处理 MQTT 提前入队的新配置。
            if (!configs_.start(error_message)) return failStart();
            if (!storage_running_.load())
            {
                error_message = "遥测存储线程在启动阶段已退出";
                return failStart();
            }
            started_ = true;
            log(common::LogLevel::Info, config_.mqtt.enabled
                ? "网关基础线程已启动，正在恢复配置并接收 MQTT 配置"
                : "MQTT 已禁用，仅恢复本地配置并采集入库");
            return true;
        }
        catch (const std::exception& error) { error_message = error.what(); }
        catch (...) { error_message = "网关启动装配发生未知异常"; }
        return failStart();
    }

    void stop() noexcept
    {
        if (stopped_) return;
        stopped_ = true;
        started_ = false;
        shutdown_requested_.store(true);
        configs_.requestStop();
        agent_.stop();
        commands_.close();
        // 必须在等待配置线程之前关闭采集队列：配置线程可能正在等待旧 Worker
        // 的阻塞 push 返回。只有整个网关退出才允许关闭这条共享队列。
        data_queue_.close();
        configs_.stop();
        // 配置线程已退出，主线程才能接管 ConnectorManager，避免并发修改连接集合。
        connectors_.stopAll();
        acks_.close();
        // 保持发布线程运行至存储收尾完成；不承诺退出前排空所有待发送消息。
        if (storage_thread_.joinable()) storage_thread_.join();
        storage_running_.store(false);
        publisher_.stop();
        mqtt_.stop();
    }

    bool isRunning() const noexcept
    {
        return started_ && storage_running_.load() && !storage_failure_.hasFailure() &&
            configs_.isRunning() &&
            (!config_.mqtt.enabled || (publisher_.isRunning() && mqtt_.isRunning()));
    }

    bool checkHealth(std::string& error_message) const
    {
        error_message.clear();
        if (!started_) { error_message = "网关未启动或正在关闭"; return false; }
        if (!storage_running_.load() || storage_failure_.hasFailure())
        {
            error_message = storage_failure_.reason();
            if (error_message.empty()) error_message = "遥测存储线程已退出";
            return false;
        }
        if (!configs_.isRunning())
        {
            error_message = configs_.failureReason();
            if (error_message.empty()) error_message = "配置控制线程已退出";
            return false;
        }
        if (config_.mqtt.enabled && !mqtt_.isRunning())
        {
            error_message = mqtt_.failureReason();
            if (error_message.empty()) error_message = "MQTT 网络线程已退出";
            return false;
        }
        if (config_.mqtt.enabled && !publisher_.isRunning())
        {
            error_message = publisher_.failureReason();
            if (error_message.empty()) error_message = "MQTT 发布线程已退出";
            return false;
        }
        return true;
    }

private:
    bool failStart() { stop(); return false; }
    void log(common::LogLevel level, std::string_view message) noexcept
    {
        try { logger_.log(level, "GatewayRuntime", message); }
        catch (...) {}
    }

    const config::GatewayConfig config_;
    common::ILogger& logger_;
    // 单位分别为完整遥测包、配置消息、配置 ACK；后续可另行暴露容量配置。
    infrastructure::concurrency::BoundedDataQueue data_queue_{1024};
    infrastructure::concurrency::ConfigCommandQueue commands_{32};
    infrastructure::concurrency::ConfigAckQueue acks_{64};
    // 三个工作线程各用独立 SQLite 连接，连接同一个数据库文件。
    infrastructure::storage::sqlite::SQLiteMessageStorage ingestion_storage_;
    infrastructure::storage::sqlite::SQLiteMessageStorage publishing_storage_;
    infrastructure::storage::sqlite::SQLiteConfigStorage config_storage_;
    infrastructure::mqtt::MosquittoMqttClient mqtt_;
    application::connector::ConnectorFactoryRegistry registry_;
    application::connector::ConnectorManager connectors_;
    application::configuration::ConnectorConfigApplier applier_;
    application::configuration::ConfigManager configs_;
    application::agent::GatewayAgent agent_;
    application::ingestion::StorageWorker ingestion_;
    application::publishing::MqttPublisherWorker publisher_;
    std::thread storage_thread_;
    std::atomic<bool> storage_running_{false};
    std::atomic<bool> shutdown_requested_{false};
    common::RuntimeFailure storage_failure_;
    bool attempted_{false};
    bool started_{false};
    bool stopped_{false};
};

GatewayRuntime::GatewayRuntime(const config::GatewayConfig& config, common::ILogger& logger)
    : impl_(std::make_unique<Impl>(config, logger)) {}
GatewayRuntime::~GatewayRuntime() = default;
bool GatewayRuntime::start(std::string& error_message) { return impl_->start(error_message); }
void GatewayRuntime::stop() noexcept { impl_->stop(); }
bool GatewayRuntime::isRunning() const noexcept { return impl_->isRunning(); }
bool GatewayRuntime::checkHealth(std::string& error_message) const { return impl_->checkHealth(error_message); }
} // namespace industrial_gateway::bootstrap
