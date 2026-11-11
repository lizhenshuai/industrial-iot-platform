#pragma once

#include "../collection/PollScheduler.hpp"
#include "../collection/TelemetryPacketBuilder.hpp"
#include "industrial_gateway/application/ports/IDataQueue.hpp"
#include "industrial_gateway/application/ports/IProtocolConnector.hpp"
#include "industrial_gateway/common/ILogger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


namespace industrial_gateway::application::connector
{

// 一个连接实例对应一个采集线程。
// 独占协议驱动，串行完成连接、调度、读取、组包和入队。
// 不解析协议配置，不访问 SQLite 或 MQTT，不关闭共享 DataQueue。
//
// 配置变化由上层准备新 Worker，停止旧 Worker 后替换，不并发修改运行任务。
// 全局停止时，上层先关闭共享 DataQueue，再调用所有 ConnectionWorker::stop()。
// 单连接替换不得关闭共享队列，仍受下述 push() 阻塞限制。
// 队列、日志必须存活到 stop() 完成；生命周期方法不得从本 Worker
// 的驱动或日志调用栈中重入，只允许由上层控制线程调用。
class ConnectionWorker final
{
public:
    // 转移驱动所有权，保存任务副本；构造时不连接设备、不启动线程。
    // 驱动不能为空，任务不能为空，且全部属于驱动的 connectionId()。
    // 任务的周期、设备、点位等基础约束由 PollScheduler 校验。
    // 不合法时抛出 std::invalid_argument。
    ConnectionWorker(
        std::unique_ptr<ports::IProtocolConnector> connector,
        std::vector<domain::model::CollectionTask> tasks,
        ports::IDataQueue& data_queue,
        common::ILogger& logger
    );
    ~ConnectionWorker();

    ConnectionWorker(const ConnectionWorker&) = delete;
    ConnectionWorker& operator=(const ConnectionWorker&) = delete;
    ConnectionWorker(ConnectionWorker&&) = delete;
    ConnectionWorker& operator=(ConnectionWorker&&) = delete;

    // true 表示线程已启动或已经运行，不代表设备连接成功。
    // 成功清空 error_message；队列已关闭或线程创建失败时返回 false。
    // stop() 后，在队列仍开放时可重新启动；首次任务立即到期。
    bool start(std::string& error_message);

    // 设置停止标志、唤醒调度等待并 join，支持重复调用。
    // 同步连接/读取需等待驱动返回，不跨线程断开正在使用的连接。
    // 如果已经阻塞在 push()，仅设置停止标志不能唤醒它：
    // 必须由上层关闭队列或确保消费者继续消费，才能完成退出。
    // 析构同样遵守此条件，不保证队列持续满时可独立停止本 Worker。
    // 停止期间尚未入队的包允许放弃并记录日志，不承诺落库。
    void stop() noexcept;

    [[nodiscard]]
    bool isRunning() const noexcept;

    [[nodiscard]]
    bool isConnected() const noexcept;

    [[nodiscard]]
    const std::string& connectionId() const noexcept;

    [[nodiscard]]
    const std::string& protocolType() const noexcept;

    // 驱动提供的只读端点标识，供管理器校验连接唯一性。
    [[nodiscard]]
    const std::string& endpointKey() const noexcept;

private:
    void run();
    void waitUntil(collection::PollScheduler::TimePoint deadline);
    void log(common::LogLevel level, const std::string& message) noexcept;

    std::unique_ptr<ports::IProtocolConnector> connector_;
    collection::PollScheduler scheduler_;
    collection::TelemetryPacketBuilder packet_builder_;
    ports::IDataQueue& data_queue_;
    common::ILogger& logger_;

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex lifecycle_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wakeup_;

    inline static constexpr std::chrono::milliseconds kConnectRetryInterval{1000};
    // 共享队列关闭不会通知本条件变量，空闲时定期观察其关闭状态。
    inline static constexpr std::chrono::milliseconds kQueueCheckInterval{200};
};

} // namespace industrial_gateway::application::connector
