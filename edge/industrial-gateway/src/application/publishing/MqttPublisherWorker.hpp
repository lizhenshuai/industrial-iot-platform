#pragma once

#include "industrial_gateway/application/ports/IMessageStorage.hpp"
#include "industrial_gateway/application/ports/IMqttClient.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/common/RuntimeFailure.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace industrial_gateway::infrastructure::concurrency
{
class ConfigAckQueue;
}

namespace industrial_gateway::application::publishing
{

// 从持久化消息中串行发布完整包，只有取得 PUBACK 才删除记录。
// 存储实例由本 Worker 独占，与 StorageWorker 使用不同实例连接同一文件。
// 所有依赖必须存活至 stop() 完成。MQTT 客户端的启停和重连由上层管理。
class MqttPublisherWorker final
{
public:
    // gateway_id 必须非空且不能包含 /、+、#、空字符，否则抛出 invalid_argument。
    MqttPublisherWorker(
        ports::IMessageStorage& message_storage,
        ports::IMqttClient& mqtt_client,
        common::ILogger& logger,
        std::string gateway_id,
        infrastructure::concurrency::ConfigAckQueue* config_acks = nullptr
    );
    ~MqttPublisherWorker();

    MqttPublisherWorker(const MqttPublisherWorker&) = delete;
    MqttPublisherWorker& operator=(const MqttPublisherWorker&) = delete;
    MqttPublisherWorker(MqttPublisherWorker&&) = delete;
    MqttPublisherWorker& operator=(MqttPublisherWorker&&) = delete;

    // true 表示线程已启动（或已经运行），不代表数据库或 Broker 已就绪。
    // stop() 后可再次启动；异常退出的线程也会在下次 start() 时回收。
    bool start(std::string& error_message);

    // 可重复调用，唤醒等待并回收线程。不要求排空数据库。
    // 当前 publish()/数据库操作必须返回后才能退出；已确认 ID 尝试删除一次。
    // 生命周期方法只允许上层调用，不允许依赖在本 Worker 的调用栈内重入。
    void stop() noexcept;

    // 查询线程存活状态，不把 MQTT 离线或数据库重试视为线程退出。
    bool isRunning() const noexcept;
    std::string failureReason() const;

private:
    // 唯一发布线程处理两类消息；每次最多发送一个 ACK，防止遥测饥饿。
    // config_acks 非空时须存活到 stop() 完成，且不得有其他发布消费者。
    void publishOneConfigAck();
    void run();
    bool waitForStop(std::chrono::milliseconds duration);
    bool initializeStorage();
    bool deleteAcknowledged(const std::vector<std::int64_t>& ids);
    void log(common::LogLevel level, const std::string& message) noexcept;

    ports::IMessageStorage& message_storage_;
    ports::IMqttClient& mqtt_client_;
    common::ILogger& logger_;
    const std::string topic_;
    const std::string config_ack_topic_;
    infrastructure::concurrency::ConfigAckQueue* const config_acks_;
    std::chrono::steady_clock::time_point next_ack_attempt_{};

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    common::RuntimeFailure failure_;
    std::mutex lifecycle_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wakeup_;

    static constexpr std::size_t kBatchSize = 100;
    inline static constexpr std::chrono::milliseconds kAcknowledgementTimeout{5000};
    inline static constexpr std::chrono::milliseconds kIdleInterval{200};
    inline static constexpr std::chrono::milliseconds kRetryInterval{1000};
};

} // namespace industrial_gateway::application::publishing
