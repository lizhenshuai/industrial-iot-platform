#pragma once

#include "industrial_gateway/domain/model/CollectionTask.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>


namespace industrial_gateway::application::collection
{

// ==================================================
// 单个连接的轮询任务调度器
// ==================================================
//
// 只计算任务到期时间，不创建线程、不等待、不读取设备、不操作队列。
// 由一个 ConnectionWorker 串行调用，不支持并发访问。
// 保存任务副本，运行期间不增删或修改任务定义。
//
// 同连接的不同频率分别调度，不合并任务或数据包。
// 每次只允许一个任务处于执行状态，完成后再取下一个到期任务。
// 采用 steady_clock，不受系统 UTC 时间调整影响。
// TelemetryPacket 的 UTC 完成时间由 Worker 在采集完成后另行记录。
//
// ==================================================

class PollScheduler final
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using TaskId = std::size_t;

    // 保存并校验任务，首次到期时间设为构造时的 Clock::now()。
    // 同一调度器的任务必须属于同一连接，采集周期不能重复。
    // 周期必须大于 0 且能无损转换为 Clock::duration。
    // 同时校验设备、点位非空及重复项，不校验协议地址。
    // 非法任务抛出 std::invalid_argument。
    // 空任务集合合法，对应没有采集工作。
    explicit PollScheduler(std::vector<domain::model::CollectionTask> tasks);

    PollScheduler(const PollScheduler&) = delete;
    PollScheduler& operator=(const PollScheduler&) = delete;
    PollScheduler(PollScheduler&&) = delete;
    PollScheduler& operator=(PollScheduler&&) = delete;

    // 重置本轮运行的调度基准，所有任务在 start_time 立即到期。
    // 用于 Worker 启动或重新启动；丢弃旧的执行状态，不改变任务定义和 ID。
    // 调用方必须保证旧任务已停止，不能随后再提交旧任务的完成通知。
    void reset(TimePoint start_time = Clock::now()) noexcept;

    // 取出一个到期任务并标记为执行中；没有到期任务时返回 nullopt。
    // 优先选择计划到期时间最早的任务，相同时按构造时的输入顺序选择。
    // TaskId 是任务在构造参数中的下标，在本调度器生命周期内保持不变。
    // 已有任务执行中时抛出 std::logic_error，避免同一连接重复发起采集。
    // now 不得早于本轮运行中此前传入的调度时间，否则抛出 invalid_argument。
    [[nodiscard]]
    std::optional<TaskId> takeNextDue(TimePoint now = Clock::now());

    // 通知当前任务本轮处理结束，采集成功或设备失败都必须调用。
    // 建议在 Worker 完成组包及入队处理后调用，使入队阻塞时间也计入跳周期。
    // 这不是遥测包的 timestamp_ms，包时间仍取采集完成时的 UTC 时间。
    //
    // 下一次到期保持原来的周期相位，并严格晚于 finished_at：
    // 周期 1 秒、原计划第 1 秒执行、实际第 3.4 秒处理完，则下次为第 4 秒。
    // 不为第 2、3 秒追加补采，也不把任务改成第 4.4 秒到期。
    //
    // 错误或非执行中的 ID 抛出 std::logic_error。
    // finished_at 早于此前调度时间抛出 std::invalid_argument。
    // 下一次到期超出时钟可表示范围时抛出 std::overflow_error。
    void completeTask(TaskId task_id, TimePoint finished_at = Clock::now());

    // 返回最早的计划到期时间，可能已到期，由 Worker 决定立即执行或等待。
    // 空任务集合或任务正在执行时返回 nullopt，不在内部 sleep。
    [[nodiscard]]
    std::optional<TimePoint> nextWakeup() const noexcept;

    // 返回任务只读引用；越界 ID 抛出 std::out_of_range。
    // 引用在调度器存活期间有效，reset() 不使其失效。
    [[nodiscard]]
    const domain::model::CollectionTask& task(TaskId task_id) const;

    [[nodiscard]]
    std::size_t taskCount() const noexcept;

private:
    struct Entry
    {
        domain::model::CollectionTask task;
        Clock::duration period;
        TimePoint next_due;
    };

    std::vector<Entry> entries_;
    std::optional<TaskId> active_task_;
    TimePoint last_observed_{};
};

} // namespace industrial_gateway::application::collection
