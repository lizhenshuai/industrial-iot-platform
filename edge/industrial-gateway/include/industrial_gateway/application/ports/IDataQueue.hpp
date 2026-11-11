
#pragma once

#include "industrial_gateway/domain/model/TelemetryPacket.hpp"

#include <chrono>
#include <cstddef>
#include <vector>


namespace industrial_gateway::application::ports
{

// ==================================================
// DataQueue 出队状态
// ==================================================

enum class DataQueuePopStatus
{
    DataAvailable,  // 成功获取到数据
    Timeout,        // 等待超时，没有获取到数据
    Closed          // 队列已关闭，且剩余数据已经取完
};


// ==================================================
// 采集数据队列接口
// ==================================================
//
// 职责：
// 1. 接收多个协议 Worker 提交的采集数据。
// 2. 为 StorageWorker 提供批量获取数据的能力。
// 3. 提供线程安全的队列操作接口。
//
// 并发模型：
// 多生产者、单消费者（MPSC）。
//
// 具体实现由 Infrastructure 层负责。
//
// ==================================================

class IDataQueue
{
public:

    virtual ~IDataQueue() = default;


    // ==================================================
    // 生产者接口
    // ==================================================

    // 提交一条采集包。
    //
    // 队列已满时阻塞等待，直到出现空位或队列关闭。
    //
    // 返回：
    // true  : 提交成功
    // false : 队列已经关闭，记录未被移动
    virtual bool push(
        domain::model::TelemetryPacket&& record
    ) = 0;


    // 批量提交采集包。
    //
    // 队列空间不足时阻塞等待，直到有足够空间容纳
    // 整个批次，或者队列关闭。
    //
    // 批量提交具有整体性：
    // 不会因为队列剩余空间不足而只提交部分数据。
    //
    // 参数：
    // records : 待提交的采集包集合
    //
    // 返回：
    // true  : 整批提交成功
    // false : 队列已经关闭，记录未被移动
    //
    // 特殊情况：
    // 1. 空批次视为成功的空操作。
    // 2. 批次大小超过队列最大容量时，抛出
    //    std::invalid_argument。
    //
    // 注意：
    // 只有确认队列能够接收整个批次后，
    // 才允许移动 records 中的数据。
    virtual bool pushBatch(
        std::vector<domain::model::TelemetryPacket>&& records
    ) = 0;


    // ==================================================
    // 消费者接口
    // ==================================================

    // 批量获取采集包。
    //
    // 参数：
    // batch          : 输出的数据集合
    // max_batch_size : 本次最多获取的数据包数，必须大于 0
    // timeout        : 最大等待时间，不能为负数
    //
    // 返回：
    // DataAvailable : 成功获取数据
    // Timeout       : 等待超时
    // Closed        : 队列已关闭且剩余数据已全部取完
    //
    // 每次调用都会先清空 batch。
    virtual DataQueuePopStatus waitPopBatch(
        std::vector<domain::model::TelemetryPacket>& batch,
        std::size_t max_batch_size,
        std::chrono::milliseconds timeout
    ) = 0;


    // ==================================================
    // 队列生命周期管理
    // ==================================================

    // 关闭队列，禁止继续提交新数据。
    // 已经进入队列的数据仍然允许被消费。
    // 重复调用应当安全。
    virtual void close() = 0;


    // 判断队列是否已经关闭。
    virtual bool isClosed() const = 0;


    // ==================================================
    // 队列状态查询
    // ==================================================

    // 当前队列中的完整包数量。
    virtual std::size_t size() const = 0;

    // 队列最大容量（包数）。
    virtual std::size_t capacity() const = 0;
};

} // namespace industrial_gateway::application::ports
