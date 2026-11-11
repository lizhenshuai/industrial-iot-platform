#include "ConnectionWorker.hpp"

#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>


namespace industrial_gateway::application::connector
{
namespace
{
using Scheduler = collection::PollScheduler;
using Clock = Scheduler::Clock;

// 使用 Linux 系统随机源生成 128 位随机 ID，不依赖可能回拨的 UTC 时钟。
// 不同 Worker、不同进程及重新启动不会重用同一个本地自增计数序列。
// ID 唯一性依赖随机碰撞概率，不宣称数学上的绝对唯一。
// 随机源暂不可用时直接报错，不退化为可能重复的时间戳 ID，也不无限等待。
std::string makeMessageId()
{
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, GRND_NONBLOCK);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "无法生成遥测包随机 ID");
        }
        if (count == 0) throw std::runtime_error("系统随机源未返回数据");
        offset += static_cast<std::size_t>(count);
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        id.push_back(hex[byte >> 4]);
        id.push_back(hex[byte & 0x0f]);
    }
    return id;
}

std::int64_t utcNowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace


ConnectionWorker::ConnectionWorker(
    std::unique_ptr<ports::IProtocolConnector> connector,
    std::vector<domain::model::CollectionTask> tasks,
    ports::IDataQueue& data_queue,
    common::ILogger& logger
)
    : connector_(std::move(connector)), scheduler_(std::move(tasks)),
      data_queue_(data_queue), logger_(logger)
{
    if (!connector_) throw std::invalid_argument("ConnectionWorker 的协议驱动不能为空");
    if (connector_->endpointKey().find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
        connector_->endpointKey().find('\0') != std::string::npos)
        throw std::invalid_argument("ConnectionWorker 的驱动端点标识不能为空或包含空字符");
    if (scheduler_.taskCount() == 0) throw std::invalid_argument("ConnectionWorker 至少需要一个采集任务");
    for (Scheduler::TaskId i = 0; i < scheduler_.taskCount(); ++i)
    {
        if (scheduler_.task(i).connection_id != connector_->connectionId())
            throw std::invalid_argument("采集任务的 connection_id 与协议驱动不一致");
    }
}

ConnectionWorker::~ConnectionWorker()
{
    stop();
}

bool ConnectionWorker::start(std::string& error_message)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    error_message.clear();
    if (running_.load()) return true;
    if (worker_thread_.joinable()) worker_thread_.join();
    if (data_queue_.isClosed())
    {
        error_message = "DataQueue 已关闭，不能启动采集线程";
        return false;
    }

    stop_requested_.store(false);
    running_.store(true);
    try
    {
        worker_thread_ = std::thread([this]()
        {
            try
            {
                run();
            }
            catch (const std::exception& error)
            {
                // 设备通信失败应通过 CollectionResult 返回。
                // 执行异常结束本 Worker，不让异常逃出线程而终止进程。
                try
                {
                    log(common::LogLevel::Critical,
                        std::string("采集线程异常退出，当前未入队数据不保证保留: ") + error.what());
                }
                catch (...) {}
            }
            catch (...)
            {
                try { log(common::LogLevel::Critical, "采集线程因未知异常退出"); }
                catch (...) {}
            }
            // 连接资源始终在采集线程结束读取后释放，stop() 只负责通知和 join。
            connector_->disconnect();
            running_.store(false);
        });
    }
    catch (const std::exception& error)
    {
        running_.store(false);
        error_message = std::string("创建采集线程失败: ") + error.what();
        return false;
    }
    return true;
}

void ConnectionWorker::stop() noexcept
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> wait_lock(wait_mutex_);
        stop_requested_.store(true);
    }
    wakeup_.notify_all();
    // 不关闭共享队列，也不从当前控制线程调用驱动 disconnect()。
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool ConnectionWorker::isRunning() const noexcept
{
    return running_.load();
}

bool ConnectionWorker::isConnected() const noexcept
{
    return connector_->isConnected();
}

const std::string& ConnectionWorker::connectionId() const noexcept
{
    return connector_->connectionId();
}

const std::string& ConnectionWorker::protocolType() const noexcept
{
    return connector_->protocolType();
}

const std::string& ConnectionWorker::endpointKey() const noexcept
{
    return connector_->endpointKey();
}

void ConnectionWorker::waitUntil(Scheduler::TimePoint deadline)
{
    const auto queue_check = Clock::now() + kQueueCheckInterval;
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wakeup_.wait_until(lock, std::min(deadline, queue_check),
                       [this]() { return stop_requested_.load(); });
}

void ConnectionWorker::log(common::LogLevel level, const std::string& message) noexcept
{
    try
    {
        logger_.log(level, "ConnectionWorker", "[" + connectionId() + "] " + message);
    }
    catch (...) {}
}

void ConnectionWorker::run()
{
    log(common::LogLevel::Info, "采集线程启动");
    scheduler_.reset();
    auto next_connect_attempt = Clock::now();
    bool connection_failure_logged = false;

    // 每个频率、每个设备分别记录故障状态，持续同一故障不每轮刷日志。
    using FailureKey = std::pair<std::chrono::milliseconds::rep, std::int64_t>;
    std::map<FailureKey, std::string> failures;
    std::vector<domain::model::CollectionResult> results;

    while (!stop_requested_.load() && !data_queue_.isClosed())
    {
        if (!connector_->isConnected() && Clock::now() >= next_connect_attempt)
        {
            std::string error;
            const bool connected = connector_->connect(error);
            next_connect_attempt = Clock::now() + kConnectRetryInterval;
            if (!connected)
            {
                connector_->disconnect();
                if (!connection_failure_logged)
                    log(common::LogLevel::Warn, "连接失败，将重试；到期任务仍生成失败结果: " + error);
                connection_failure_logged = true;
            }
            else
            {
                if (!connector_->isConnected())
                    throw std::runtime_error("驱动 connect() 返回成功，但连接状态仍为未连接");
                if (connection_failure_logged) log(common::LogLevel::Info, "设备连接已恢复");
                connection_failure_logged = false;
            }
        }
        if (stop_requested_.load() || data_queue_.isClosed()) break;

        const auto task_id = scheduler_.takeNextDue();
        if (!task_id)
        {
            const auto next_task = scheduler_.nextWakeup();
            if (!next_task) throw std::logic_error("非空调度器没有下一次到期时间");
            const auto deadline = connector_->isConnected()
                ? *next_task : std::min(*next_task, next_connect_attempt);
            waitUntil(deadline);
            continue;
        }

        const auto& task = scheduler_.task(*task_id);
        results.clear();
        std::string error;
        const bool was_connected = connector_->isConnected();
        const bool processed = connector_->readPoints(task, results, error);
        // 必须紧跟采集返回记录时间，不能等组包、日志或阻塞入队之后再取。
        const auto completed_at_ms = utcNowMilliseconds();
        if (was_connected && !connector_->isConnected())
        {
            next_connect_attempt = Clock::now() + kConnectRetryInterval;
            if (!connection_failure_logged) log(common::LogLevel::Warn, "采集期间连接断开，等待重连");
            connection_failure_logged = true;
        }
        if (!processed)
        {
            // 非法任务/映射错误不是普通设备离线，停止本 Worker 避免无限重试坏配置。
            log(common::LogLevel::Critical, "驱动拒绝采集任务，停止线程: " + error);
            break;
        }
        if (stop_requested_.load() || data_queue_.isClosed())
        {
            log(common::LogLevel::Warn, "停止期间本轮采集结果尚未入队，放弃本轮结果");
            break;
        }

        domain::model::TelemetryPacket packet;
        const auto message_id = makeMessageId();
        if (!packet_builder_.build(task, results, message_id, completed_at_ms, packet, error))
        {
            log(common::LogLevel::Critical, "采集结果不符合组包约定，停止线程: " + error);
            break;
        }

        for (const auto& result : results)
        {
            const FailureKey key{task.interval.count(), result.device_id};
            const auto label = "周期=" + std::to_string(task.interval.count()) +
                               "ms，device_id=" + std::to_string(result.device_id);
            if (result.status == domain::enums::CollectionStatus::Failure)
            {
                const auto found = failures.find(key);
                if (found == failures.end() || found->second != result.error_message)
                    log(common::LogLevel::Warn, label + " 采集失败: " + result.error_message);
                failures[key] = result.error_message;
            }
            else if (failures.erase(key) != 0)
            {
                log(common::LogLevel::Info, label + " 采集已恢复");
            }
        }

        if (stop_requested_.load())
        {
            log(common::LogLevel::Warn, "停止期间放弃尚未入队的包，message_id=" + message_id);
            break;
        }
        // push 在满队列上阻塞；不会为等待中的包重新生成 ID 或完成时间。
        // 只有上层关闭共享队列或消费者腾出空间，才能解除这次阻塞。
        if (!data_queue_.push(std::move(packet)))
        {
            log(common::LogLevel::Warn, "DataQueue 已关闭，包未入队，message_id=" + message_id);
            break;
        }
        // 包时间保持采集完成时间；调度时间则包含组包及入队阻塞，跳过错过轮次。
        scheduler_.completeTask(*task_id);
    }

    log(common::LogLevel::Info, "采集线程退出");
}

} // namespace industrial_gateway::application::connector
