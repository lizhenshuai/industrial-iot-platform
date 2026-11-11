#include "ConfigAckQueue.hpp"
#include <stdexcept>

namespace industrial_gateway::infrastructure::concurrency
{
ConfigAckQueue::ConfigAckQueue(std::size_t capacity) : capacity_(capacity)
{
    if (capacity_ == 0) throw std::invalid_argument("ConfigAckQueue 容量必须大于 0");
}
bool ConfigAckQueue::tryPush(const ::industrial_gateway::config::ConfigApplyResult& result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || queue_.size() >= capacity_) return false;
    queue_.push_back(result);
    return true;
}
bool ConfigAckQueue::tryPeek(::industrial_gateway::config::ConfigApplyResult& result) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    result = queue_.front();
    return true;
}
void ConfigAckQueue::popFront()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_.empty()) queue_.pop_front();
}
void ConfigAckQueue::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}
std::size_t ConfigAckQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}
} // namespace industrial_gateway::infrastructure::concurrency
