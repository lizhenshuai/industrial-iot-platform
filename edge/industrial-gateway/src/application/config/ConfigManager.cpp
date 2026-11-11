#include "ConfigManager.hpp"
#include "ConnectorConfigApplier.hpp"
#include "../../infrastructure/concurrency/ConfigCommandQueue.hpp"
#include "../../infrastructure/concurrency/ConfigAckQueue.hpp"
#include "../../infrastructure/config/CollectionConfigParser.hpp"

#include <chrono>
#include <exception>
#include <optional>

namespace industrial_gateway::application::configuration
{
namespace
{
using ApplyStatus = ::industrial_gateway::config::ConfigApplyStatus;
constexpr std::chrono::milliseconds kQueueWaitTimeout{100};
constexpr std::chrono::milliseconds kStorageRetryInterval{1000};
constexpr std::chrono::seconds kHealthCheckInterval{10};
}

ConfigManager::ConfigManager(infrastructure::concurrency::ConfigCommandQueue& config_queue,
    ports::IConfigStorage& storage, ConnectorConfigApplier& applier,
    infrastructure::concurrency::ConfigAckQueue& ack_queue, common::ILogger& logger)
    : config_queue_(config_queue), storage_(storage), applier_(applier),
      ack_queue_(ack_queue), logger_(logger) {}

ConfigManager::~ConfigManager() { stop(); }

bool ConfigManager::start(std::string& error_message)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    error_message.clear();
    if (running_.load()) return true;
    if (worker_thread_.joinable()) worker_thread_.join();
    if (config_queue_.isClosed())
    {
        error_message = "配置队列已关闭，不能启动 ConfigManager";
        return false;
    }
    failure_.reset();
    stop_requested_.store(false);
    running_.store(true);
    try
    {
        worker_thread_ = std::thread([this]()
        {
            try { run(); }
            catch (const std::exception& error)
            {
                failure_.record("配置管理线程异常退出", error.what());
                log(common::LogLevel::Critical, error.what());
            }
            catch (...)
            {
                failure_.record("配置管理线程因未知异常退出");
                log(common::LogLevel::Critical, "配置管理线程异常退出");
            }
            if (!stop_requested_.load())
                failure_.record("配置管理线程未经停止请求退出");
            applier_.discard();
            running_.store(false);
        });
    }
    catch (const std::exception& error)
    {
        running_.store(false);
        error_message = error.what();
        return false;
    }
    return true;
}

void ConfigManager::stop() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    requestStop();
    if (worker_thread_.joinable()) worker_thread_.join();
}

void ConfigManager::requestStop() noexcept
{
    {
        std::lock_guard<std::mutex> wait_lock(wait_mutex_);
        stop_requested_.store(true);
    }
    wakeup_.notify_all();
}

bool ConfigManager::isRunning() const noexcept { return running_.load() && !failure_.hasFailure(); }
std::string ConfigManager::failureReason() const { return failure_.reason(); }

bool ConfigManager::restore()
{
    // 初始化或读取失败时先重试，不把“读取失败”当成“没有配置”。
    // 只有完成这次读取才开始消费新消息，恢复不会在新配置之后再次覆盖运行状态。
    while (!stop_requested_.load())
    {
        std::string error;
        std::optional<ports::StoredConfig> stored;
        if (storage_.initialize(error) && storage_.load(stored, error))
        {
            if (stop_requested_.load()) return false;
            if (stored)
            {
                const auto result = process(stored->config_json, false);
                if (result.status != ApplyStatus::Applied)
                {
                    log(common::LogLevel::Error, failure_.hasFailure()
                        ? "本地配置应用失败，保留记录并通知主线程故障退出"
                        : "本地配置校验失败，保留记录并等待平台修正或重发");
                    log(common::LogLevel::Error, result.error_message);
                }
            }
            return !failure_.hasFailure();
        }
        log(common::LogLevel::Error, "配置存储初始化或读取失败，将重试");
        log(common::LogLevel::Error, error);
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wakeup_.wait_for(lock, kStorageRetryInterval, [this]() { return stop_requested_.load(); });
    }
    return false;
}

void ConfigManager::run()
{
    if (!restore()) return;
    using infrastructure::concurrency::ConfigCommandPopStatus;
    auto next_check = std::chrono::steady_clock::now() + kHealthCheckInterval;
    while (!stop_requested_.load() && !failure_.hasFailure())
    {
        // 只在本控制线程检查连接，且不在配置替换中间采样。
        // 不因新配置到来重置检查周期，避免持续消息让健康检查饥饿。
        if (std::chrono::steady_clock::now() >= next_check)
        {
            std::string error;
            if (!applier_.checkRunningConnections(error))
            {
                if (!stop_requested_.load())
                {
                    failure_.record(error);
                    log(common::LogLevel::Critical, error);
                }
                break;
            }
            next_check = std::chrono::steady_clock::now() + kHealthCheckInterval;
        }
        ports::MqttMessage message;
        const auto status = config_queue_.waitPop(message, kQueueWaitTimeout);
        if (status == ConfigCommandPopStatus::Closed) break;
        if (status == ConfigCommandPopStatus::Timeout) continue;
        // 已取出的消息完成处理及结果投递；retained 消息与实时消息同样处理。
        deliverResult(process(message.payload, true));
    }
}

ConfigManager::ApplyResult ConfigManager::process(const std::string& payload, bool persist)
{
    ApplyResult result;
    bool applying = false;
    try
    {
        ::industrial_gateway::config::CollectionConfigSnapshot snapshot;
        if (!infrastructure::config::CollectionConfigParser{}.parse(
                payload, snapshot, result.error_message, &result.request_id))
        {
            result.status = ApplyStatus::Rejected;
            return result;
        }
        result.request_id = snapshot.request_id;
        if (!applier_.prepare(snapshot, result.error_message))
        {
            result.status = ApplyStatus::Rejected;
            if (result.error_message.empty()) result.error_message = "协议配置准备失败";
            return result;
        }
        if (persist)
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            // 保存整份消息文本，包含全部协议连接；保存失败不改变任何运行连接。
            if (!storage_.initialize(result.error_message) ||
                !storage_.save(ports::StoredConfig{now, payload}, result.error_message))
            {
                applier_.discard();
                if (result.error_message.empty()) result.error_message = "配置保存失败";
                return result;
            }
        }
        // 同内容重发也经过 prepare；仍在运行的未变化连接会跳过，失败连接会重建。
        applying = true;
        if (!applier_.apply(result.error_message))
        {
            if (result.error_message.empty()) result.error_message = "配置应用失败";
            // 此时可能已停掉旧连接或只启动部分新连接，不能继续宣称运行正常。
            // 校验拒绝和保存失败没有改变运行集合，不走此故障退出路径。
            if (!stop_requested_.load())
                failure_.record("采集配置应用失败", result.error_message);
            return result;
        }
        result.status = ApplyStatus::Applied;
        result.error_message.clear();
    }
    catch (...)
    {
        applier_.discard();
        result.status = ApplyStatus::Failed;
        result.error_message = "配置解析、保存或应用发生执行异常";
        if (applying && !stop_requested_.load())
            failure_.record("采集配置应用期间发生执行异常");
    }
    return result;
}

void ConfigManager::deliverResult(const ApplyResult& result) noexcept
{
    try
    {
        if (!ack_queue_.tryPush(result))
            log(common::LogLevel::Error, "配置 ACK 队列满或已关闭，结果未入队；平台需按请求超时重发");
    }
    catch (...) { log(common::LogLevel::Error, "配置 ACK 入队异常，应用结果不回滚"); }
}

void ConfigManager::log(common::LogLevel level, std::string_view message) noexcept
{
    try { logger_.log(level, "ConfigManager", message); }
    catch (...) {}
}
} // namespace industrial_gateway::application::configuration
