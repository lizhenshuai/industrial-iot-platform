#include "ConfigCommandQueue.hpp"

#include <stdexcept>
#include <utility>


namespace industrial_gateway::infrastructure::concurrency
{

ConfigCommandQueue::ConfigCommandQueue(std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0)
        throw std::invalid_argument("ConfigCommandQueue capacity 必须大于 0");
}

ConfigCommandPushStatus ConfigCommandQueue::tryPush(
    const application::ports::MqttMessage& message
)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return ConfigCommandPushStatus::Closed;
        if (queue_.size() >= capacity_) return ConfigCommandPushStatus::Full;
        queue_.push_back(message);
    }
    not_empty_.notify_one();
    return ConfigCommandPushStatus::Accepted;
}

ConfigCommandPopStatus ConfigCommandQueue::waitPop(
    application::ports::MqttMessage& message,
    std::chrono::milliseconds timeout
)
{
    if (timeout.count() < 0)
        throw std::invalid_argument("ConfigCommandQueue 等待超时不能为负数");

    std::unique_lock<std::mutex> lock(mutex_);
    if (!not_empty_.wait_for(lock, timeout, [this]()
        { return closed_ || !queue_.empty(); }))
    {
        return ConfigCommandPopStatus::Timeout;
    }
    if (queue_.empty()) return ConfigCommandPopStatus::Closed;

    message = std::move(queue_.front());
    queue_.pop_front();
    return ConfigCommandPopStatus::MessageAvailable;
}

void ConfigCommandQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
}

bool ConfigCommandQueue::isClosed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t ConfigCommandQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t ConfigCommandQueue::capacity() const noexcept
{
    return capacity_;
}

} // namespace industrial_gateway::infrastructure::concurrency
