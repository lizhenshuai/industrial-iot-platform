#pragma once

#include "industrial_gateway/application/ports/IMqttClient.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>


namespace industrial_gateway::infrastructure::concurrency
{

enum class ConfigCommandPushStatus
{
    Accepted,
    Full,
    Closed
};

enum class ConfigCommandPopStatus
{
    MessageAvailable,
    Timeout,
    Closed
};

// ==================================================
// 配置消息队列：多生产者、单消费者、有界 FIFO
// ==================================================
//
// 保存拥有独立字符串副本的 MqttMessage，包含原始 payload 和接收元数据。
// 入队时不解析 JSON、不合并快照、不判断配置版本，也不执行配置应用。
// Accepted 只表示消息进入内存队列，不表示持久化或配置应用成功。
//
// ==================================================

class ConfigCommandQueue final
{
public:
    // capacity 为消息条数上限，必须大于 0，否则抛出 invalid_argument。
    // 不限制单条 payload 的字节数，消息大小限制由接收策略另外确定。
    explicit ConfigCommandQueue(std::size_t capacity);

    // 销毁前必须结束所有队列操作及消费者等待，close() 本身不负责 join。
    ~ConfigCommandQueue() = default;

    ConfigCommandQueue(const ConfigCommandQueue&) = delete;
    ConfigCommandQueue& operator=(const ConfigCommandQueue&) = delete;
    ConfigCommandQueue(ConfigCommandQueue&&) = delete;
    ConfigCommandQueue& operator=(ConfigCommandQueue&&) = delete;

    // 不等待队列空位；仅在队列操作期间持有互斥锁。
    // Full / Closed 时不复制消息，不移除或覆盖已经排队的消息。
    // 成功时复制消息，调用方可在返回后释放 MQTT 回调的原始消息。
    // 内存分配异常交给调用方处理，不伪装成正常入队成功。
    [[nodiscard]]
    ConfigCommandPushStatus tryPush(const application::ports::MqttMessage& message);

    // 等待单条消息；timeout 不得为负，0 表示只检查一次。
    // MessageAvailable 时更新 message；Timeout / Closed 时保持输出不变。
    // 关闭后仍可按 FIFO 取出已有消息，排空后返回 Closed。
    [[nodiscard]]
    ConfigCommandPopStatus waitPop(
        application::ports::MqttMessage& message,
        std::chrono::milliseconds timeout
    );

    // 永久关闭入队并唤醒等待者，可重复调用，不清空已有消息。
    // 是否在关闭期间继续处理配置，由消费者的退出策略决定。
    void close();

    [[nodiscard]] bool isClosed() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<application::ports::MqttMessage> queue_;
    const std::size_t capacity_;
    bool closed_{false};
};

} // namespace industrial_gateway::infrastructure::concurrency
