#pragma once

#include "industrial_gateway/config/ConfigApplyResult.hpp"
#include <cstddef>
#include <deque>
#include <mutex>

namespace industrial_gateway::infrastructure::concurrency
{
// 有界内存 ACK 队列。多生产者、唯一发布消费者；不保存配置历史。
// 消费者先读取队首，收到 PUBACK 后才移除；失败保留原结果以便重发。
class ConfigAckQueue final
{
public:
    explicit ConfigAckQueue(std::size_t capacity);
    bool tryPush(const ::industrial_gateway::config::ConfigApplyResult& result);
    bool tryPeek(::industrial_gateway::config::ConfigApplyResult& result) const;
    // 仅唯一消费者调用，在成功发布或确认队首数据损坏后移除。
    void popFront();
    void close(); // 禁止新入队，保留队首及已有数据供发送。
    std::size_t size() const;
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<::industrial_gateway::config::ConfigApplyResult> queue_;
    bool closed_{false};
};
} // namespace industrial_gateway::infrastructure::concurrency
