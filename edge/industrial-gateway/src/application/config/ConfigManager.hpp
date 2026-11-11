#pragma once

#include "industrial_gateway/application/ports/IConfigStorage.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/common/RuntimeFailure.hpp"
#include "industrial_gateway/config/ConfigApplyResult.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace industrial_gateway::infrastructure::concurrency
{
class ConfigCommandQueue;
class ConfigAckQueue;
}

namespace industrial_gateway::application::configuration
{
class ConnectorConfigApplier;

// 配置控制线程：启动读取唯一记录，然后按 FIFO 处理平台完整快照。
// 流程为解析 -> 全部准备 -> SQLite 覆盖 -> 应用 -> ACK 入队。
// 无配置版本、历史记录或数据库应用状态；request_id 只用于回复关联。
// 所有依赖由外部装配，生命周期必须覆盖本对象及其线程。
// Applier 及其 ConnectorManager 在本线程运行期间不得由外部并发操作。
class ConfigManager final
{
public:
    ConfigManager(infrastructure::concurrency::ConfigCommandQueue& config_queue,
                  ports::IConfigStorage& storage, ConnectorConfigApplier& applier,
                  infrastructure::concurrency::ConfigAckQueue& ack_queue,
                  common::ILogger& logger);
    ~ConfigManager();
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    // true 仅表示线程启动，恢复与应用都在控制线程中进行。
    // 每次 start 都先恢复 SQLite 配置，再消费已排队的新消息，避免旧配置晚到覆盖。
    bool start(std::string& error_message);
    // 完成已经取出的请求后退出，未取出的消息留在队列；不停止采集连接。
    // 不关闭 MQTT、配置队列或 ACK 队列。现有 Worker 停止阻塞限制仍适用。
    // 不得从依赖回调、日志或本线程中调用生命周期方法。
    void stop() noexcept;
    // 全局关闭时先通知，随后关闭共享队列，最后 stop() 等待线程回收。
    // 不等待 join，避免把正常关闭造成的 Worker 退出识别为故障。
    void requestStop() noexcept;
    bool isRunning() const noexcept;
    std::string failureReason() const;
private:
    using ApplyResult = ::industrial_gateway::config::ConfigApplyResult;
    void run();
    bool restore();
    // persist=false 用于恢复：不改保存时间，也不回复历史请求 ACK。
    ApplyResult process(const std::string& payload, bool persist);
    void deliverResult(const ApplyResult& result) noexcept;
    void log(common::LogLevel level, std::string_view message) noexcept;

    infrastructure::concurrency::ConfigCommandQueue& config_queue_;
    ports::IConfigStorage& storage_;
    ConnectorConfigApplier& applier_;
    infrastructure::concurrency::ConfigAckQueue& ack_queue_;
    common::ILogger& logger_;
    std::thread worker_thread_;
    std::mutex lifecycle_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wakeup_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    common::RuntimeFailure failure_;
};
} // namespace industrial_gateway::application::configuration
