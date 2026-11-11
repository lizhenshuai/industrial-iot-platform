
#include "BoundedDataQueue.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>


namespace industrial_gateway::infrastructure::concurrency
{

// ==================================================
// 构造与析构
// ==================================================
// 构造函数，只做了一件事，就是赋值容量大小
BoundedDataQueue::BoundedDataQueue(std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0)
    {
        throw std::invalid_argument(
            "DataQueue capacity must be greater than zero"
        );
    }
}


// ==================================================
// 生产者接口
// ==================================================

bool BoundedDataQueue::push(
    domain::model::TelemetryPacket&& record
)
{
    std::unique_lock<std::mutex> lock(mutex_);

    // 队列满时等待，直到出现空位或队列关闭，队列满，就在这阻塞，等在这
    not_full_.wait(
        lock,
        [this]()
        {
            return closed_ || queue_.size() < capacity_;
        }
    );

    // 队列已经关闭，不再接收新数据
    // 此时 record 尚未移动
    if (closed_)
    {
        return false;
    }

    // 将数据移动到队列末尾
    queue_.emplace_back(std::move(record));

    // 释放锁
    lock.unlock();

    // 通知消费者：队列中出现了新数据
    not_empty_.notify_one();

    return true;
}


// 批量入队
bool BoundedDataQueue::pushBatch(
    std::vector<domain::model::TelemetryPacket>&& records
)
{
    // 空批次视为成功的空操作
    if (records.empty())
    {
        return true;
    }

    // 批次超过队列最大容量，无法一次性提交
    if (records.size() > capacity_)
    {
        throw std::invalid_argument(
            "Batch size exceeds DataQueue capacity"
        );
    }

    std::unique_lock<std::mutex> lock(mutex_);

    // 等待队列出现足够空间，或者队列关闭
    //
    // 必须保证有足够空间容纳整个批次，
    // 不允许部分入队。
    not_full_.wait(
        lock,
        [this, &records]()
        {
            return closed_ ||
                   records.size() <= capacity_ - queue_.size();
        }
    );

    // 队列已经关闭，不接受数据
    // 此时 records 中的数据尚未移动
    if (closed_)
    {
        return false;
    }

    // 将整个批次按照原有顺序移动到队列末尾
    //
    // 持有互斥锁期间完成插入，
    // 保证其他生产者不能在批次中间插入数据。
    queue_.insert(
        queue_.end(),
        std::make_move_iterator(records.begin()),
        std::make_move_iterator(records.end())
    );

    // 释放锁
    lock.unlock();

    // 通知消费者：队列中出现了一批新数据
    not_empty_.notify_one();

    return true;
}


// ==================================================
// 消费者接口
// ==================================================

application::ports::DataQueuePopStatus
BoundedDataQueue::waitPopBatch(
    std::vector<domain::model::TelemetryPacket>& batch,
    std::size_t max_batch_size,
    std::chrono::milliseconds timeout
)
{
    // 每次调用先清空输出容器
    batch.clear();

    // 检查参数
    if (max_batch_size == 0)
    {
        throw std::invalid_argument(
            "max_batch_size must be greater than zero"
        );
    }

    if (timeout.count() < 0)
    {
        throw std::invalid_argument(
            "timeout must not be negative"
        );
    }

    std::unique_lock<std::mutex> lock(mutex_);

    // 等待数据到达，或者队列关闭
    const bool ready = not_empty_.wait_for(
        lock,
        timeout,
        [this]()
        {
            return closed_ || !queue_.empty();
        }
    );

    // 等待超时，且没有数据可消费
    if (!ready)
    {
        return application::ports::DataQueuePopStatus::Timeout;
    }

    // 队列已关闭，并且剩余数据已经全部消费
    if (queue_.empty())
    {
        return application::ports::DataQueuePopStatus::Closed;
    }

    // 计算本次实际出队数量
    const std::size_t batch_size = std::min(
        max_batch_size,
        queue_.size()
    );

    // 提前分配空间，避免出队过程中 vector 扩容
    batch.reserve(batch_size);

    // 按照 FIFO 顺序批量移动数据
    for (std::size_t i = 0; i < batch_size; ++i)
    {
        batch.emplace_back(std::move(queue_.front()));

        queue_.pop_front();
    }

    // 释放锁
    lock.unlock();

    // 批量出队释放了多个位置，唤醒等待中的生产者
    not_full_.notify_all();

    return application::ports::DataQueuePopStatus::DataAvailable;
}


// ==================================================
// 生命周期管理
// ==================================================

void BoundedDataQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 标记队列关闭
        closed_ = true;
    }

    // 唤醒所有等待中的生产者和消费者
    not_empty_.notify_all();

    not_full_.notify_all();
}


// ==================================================
// 队列状态查询
// ==================================================

bool BoundedDataQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return closed_;
}


std::size_t BoundedDataQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return queue_.size();
}


std::size_t BoundedDataQueue::capacity() const
{
    return capacity_;
}

} // namespace industrial_gateway::infrastructure::concurrency
