#include "PollScheduler.hpp"

#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>


namespace industrial_gateway::application::collection
{
namespace
{
using Clock = PollScheduler::Clock;

void require(bool valid, const std::string& reason)
{
    if (!valid) throw std::invalid_argument("PollScheduler: " + reason);
}

bool validName(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos &&
           value.find('\0') == std::string::npos;
}

Clock::duration toPeriod(std::chrono::milliseconds interval)
{
    using Duration = Clock::duration;
    // 当前网关平台的 steady_clock 使用有符号整数计数，精度不低于毫秒。
    static_assert(std::is_integral_v<Duration::rep> && std::is_signed_v<Duration::rep>,
                  "PollScheduler requires signed integral steady_clock ticks");
    static_assert(std::ratio_less_equal_v<Duration::period, std::milli>,
                  "PollScheduler requires at least millisecond clock resolution");

    require(interval.count() > 0, "采集周期必须大于 0");
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Duration::max());
    require(interval <= maximum, "采集周期超出单调时钟可表示范围");
    const auto period = std::chrono::duration_cast<Duration>(interval);
    require(std::chrono::duration_cast<std::chrono::milliseconds>(period) == interval,
            "采集周期无法无损转换为单调时钟精度");
    return period;
}

void validateDevices(const domain::model::CollectionTask& task)
{
    require(!task.devices.empty(), "任务设备集合不能为空");
    std::unordered_set<std::int64_t> device_ids;
    for (const auto& device : task.devices)
    {
        const auto label = "device_id=" + std::to_string(device.device_id) + ": ";
        require(device_ids.insert(device.device_id).second, label + "设备重复");
        require(!device.properties.empty(), label + "点位集合不能为空");
        std::unordered_set<std::string> properties;
        for (const auto& property : device.properties)
        {
            require(validName(property), label + "点位名称不能为空或包含空字符");
            require(properties.insert(property).second, label + "点位重复: " + property);
        }
    }
}

// 保持 scheduled 的周期相位，直接跳到 finished 之后的第一个周期。
// 不逐周期循环，也不通过 (错过次数 * 周期) 计算，避免大跨度乘法溢出。
PollScheduler::TimePoint nextAfter(PollScheduler::TimePoint scheduled,
                                   Clock::duration period,
                                   PollScheduler::TimePoint finished)
{
    using Rep = Clock::duration::rep;
    using UnsignedRep = std::make_unsigned_t<Rep>;
    const Rep start = scheduled.time_since_epoch().count();
    const Rep end = finished.time_since_epoch().count();

    // finished >= scheduled；无符号减法也能安全处理跨越有符号计数零点的跨度。
    const auto elapsed = static_cast<UnsignedRep>(end) - static_cast<UnsignedRep>(start);
    const auto remainder = elapsed % static_cast<UnsignedRep>(period.count());
    const Rep remaining = period.count() - static_cast<Rep>(remainder);
    if (end > std::numeric_limits<Rep>::max() - remaining)
        throw std::overflow_error("PollScheduler: 下一次到期时间超出单调时钟范围");
    return PollScheduler::TimePoint{Clock::duration{end + remaining}};
}

} // namespace


PollScheduler::PollScheduler(std::vector<domain::model::CollectionTask> tasks)
{
    entries_.reserve(tasks.size());
    std::unordered_set<std::chrono::milliseconds::rep> intervals;
    // 单个调度器只管理一个连接；不同连接使用各自的 Worker 和调度器。
    const std::string connection_id = tasks.empty() ? std::string{} : tasks.front().connection_id;
    for (auto& task : tasks)
    {
        require(validName(task.connection_id), "connection_id 不能为空或包含空字符");
        require(task.connection_id == connection_id, "不能混合不同连接的任务");
        const auto period = toPeriod(task.interval);
        require(intervals.insert(task.interval.count()).second,
                "同连接存在重复采集周期: " + std::to_string(task.interval.count()) + "ms");
        validateDevices(task);
        entries_.push_back({std::move(task), period, TimePoint{}});
    }
    reset();
}

void PollScheduler::reset(TimePoint start_time) noexcept
{
    active_task_.reset();
    last_observed_ = start_time;
    for (auto& entry : entries_) entry.next_due = start_time;
}

std::optional<PollScheduler::TaskId> PollScheduler::takeNextDue(TimePoint now)
{
    if (active_task_) throw std::logic_error("PollScheduler: 必须先完成当前执行中的任务");
    require(now >= last_observed_, "调度时间不能倒退");
    last_observed_ = now;

    std::optional<TaskId> selected;
    for (TaskId i = 0; i < entries_.size(); ++i)
    {
        if (entries_[i].next_due <= now &&
            (!selected || entries_[i].next_due < entries_[*selected].next_due))
            selected = i;
    }
    active_task_ = selected;
    return selected;
}

void PollScheduler::completeTask(TaskId task_id, TimePoint finished_at)
{
    if (!active_task_ || *active_task_ != task_id)
        throw std::logic_error("PollScheduler: 完成通知与当前执行任务不匹配");
    require(finished_at >= last_observed_, "任务完成时间不能早于此前调度时间");
    auto& entry = entries_[task_id];
    // 先计算，出现时间范围错误时保留现有执行状态，由上层决定终止或重置。
    const auto next = nextAfter(entry.next_due, entry.period, finished_at);
    entry.next_due = next;
    last_observed_ = finished_at;
    active_task_.reset();
}

std::optional<PollScheduler::TimePoint> PollScheduler::nextWakeup() const noexcept
{
    if (entries_.empty() || active_task_) return std::nullopt;
    auto next = entries_.front().next_due;
    for (const auto& entry : entries_)
    {
        if (entry.next_due < next) next = entry.next_due;
    }
    return next;
}

const domain::model::CollectionTask& PollScheduler::task(TaskId task_id) const
{
    return entries_.at(task_id).task;
}

std::size_t PollScheduler::taskCount() const noexcept
{
    return entries_.size();
}

} // namespace industrial_gateway::application::collection
