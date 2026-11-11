
#pragma once

#include "industrial_gateway/application/ports/IDataQueue.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>


namespace industrial_gateway::infrastructure::concurrency
{

// ==================================================
// 有界采集数据队列
// ==================================================
//
// 实现 IDataQueue 接口。
//
// 并发模型：
// Multiple Producers + Single Consumer
//
// 核心特性：
// 1. 有界 FIFO 队列
// 2. 多线程安全
// 3. 支持单个完整包入队
// 4. 支持批量数据入队
// 5. 队列满时阻塞生产者
// 6. 队列空时阻塞消费者
// 7. 支持批量出队
// 8. 支持安全关闭和剩余数据排空
//
// ==================================================

class BoundedDataQueue final
    : public application::ports::IDataQueue
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    // 创建有界队列。
    //
    // capacity 必须大于 0。
    // 具体参数校验在 cpp 中实现。
    explicit BoundedDataQueue(std::size_t capacity);

    ~BoundedDataQueue() override = default;


    // 禁止拷贝和移动。
    // 队列包含互斥锁和条件变量，不应随意转移其生命周期。
    BoundedDataQueue(const BoundedDataQueue&) = delete;

    BoundedDataQueue& operator=(
        const BoundedDataQueue&
    ) = delete;

    BoundedDataQueue(BoundedDataQueue&&) = delete;

    BoundedDataQueue& operator=(
        BoundedDataQueue&&
    ) = delete;


    // ==================================================
    // 生产者接口
    // ==================================================

    // 单个完整包入队
    bool push(
        domain::model::TelemetryPacket&& record
    ) override;


    // 批量数据入队
    //
    // 队列空间不足时阻塞等待，直到有足够空间
    // 容纳整个批次，或者队列关闭。
    //
    // 批量提交具有整体性，不允许部分提交。
    bool pushBatch(
        std::vector<domain::model::TelemetryPacket>&& records
    ) override;


    // ==================================================
    // 消费者接口
    // ==================================================

    application::ports::DataQueuePopStatus waitPopBatch(
        std::vector<domain::model::TelemetryPacket>& batch,
        std::size_t max_batch_size,
        std::chrono::milliseconds timeout
    ) override;


    // ==================================================
    // 生命周期管理
    // ==================================================

    void close() override;

    bool isClosed() const override;


    // ==================================================
    // 队列状态查询
    // ==================================================

    std::size_t size() const override;

    std::size_t capacity() const override;


private:

    // ==================================================
    // 队列数据
    // ==================================================

    // 保护队列及关闭状态
    mutable std::mutex mutex_;

    // 存放采集包
    std::deque<domain::model::TelemetryPacket> queue_;

    // 队列最大容量
    const std::size_t capacity_;

    // 是否已经关闭
    bool closed_{false};


    // ==================================================
    // 线程同步
    // ==================================================

    // 通知消费者：队列中有可消费的数据
    std::condition_variable not_empty_;

    // 通知生产者：队列中出现可用空间
    std::condition_variable not_full_;
};

} // namespace industrial_gateway::infrastructure::concurrency
