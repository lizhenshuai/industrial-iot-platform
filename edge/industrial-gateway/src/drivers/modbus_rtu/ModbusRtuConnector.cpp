#include "ModbusRtuConnector.hpp"
#include "ModbusRtuConfigParser.hpp"

#include <modbus/modbus.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <mutex>
#include <thread>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace industrial_gateway::drivers::modbus_rtu
{
namespace
{
using domain::model::CollectionResult;
using domain::model::CollectionTask;
using domain::types::TelemetryValue;
using domain::enums::CollectionStatus;

struct ContextDeleter
{
    void operator()(modbus_t* context) const noexcept
    {
        if (!context) return;
        if (modbus_get_socket(context) >= 0) modbus_close(context);
        modbus_free(context);
    }
};
using Context = std::unique_ptr<modbus_t, ContextDeleter>;

struct Timeout
{
    std::uint32_t seconds;
    std::uint32_t microseconds;
};

Timeout toTimeout(std::chrono::milliseconds value)
{
    const auto milliseconds = value.count();
    if (milliseconds <= 0 ||
        milliseconds / 1000 > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("超时必须大于 0，且秒数不能超过 libmodbus uint32_t 范围");
    return {static_cast<std::uint32_t>(milliseconds / 1000),
            static_cast<std::uint32_t>((milliseconds % 1000) * 1000)};
}

bool setTimeout(modbus_t* context, Timeout timeout)
{
    return modbus_set_response_timeout(context, timeout.seconds, timeout.microseconds) == 0;
}

std::string protocolError(const std::string& operation, int error)
{
    return operation + ": " + modbus_strerror(error) + " (errno=" + std::to_string(error) + ")";
}

// 设备存在时按字符设备号识别，符号链接或不同节点名不会绕过同端点检查。
// 设备尚未插入时保留路径标识，不能让离线启动恢复因 USB 暂未出现而失败。
std::string makeEndpointKey(const std::string& path)
{
    struct stat info{};
    if (::stat(path.c_str(), &info) == 0)
    {
        if (!S_ISCHR(info.st_mode)) throw std::invalid_argument("RTU 路径必须指向串口字符设备");
        return "serial:" + std::to_string(static_cast<unsigned long long>(info.st_rdev));
    }
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    return "serial-path:" + (error ? std::filesystem::path(path).lexically_normal() : resolved).string();
}

// 在 libmodbus 修改 termios 前先占用端口。设备号锁覆盖本进程的别名节点，
// flock 协调合作进程；之后再以 TIOCEXCL 阻止其他普通进程打开端口。
// 不承诺排除已打开端口或具有管理员权限的非合作程序。
class SerialLease final
{
public:
    ~SerialLease()
    {
        if (fd >= 0) ::close(fd); // close 同时释放 flock。
        if (claimed_)
        {
            std::lock_guard<std::mutex> lock(ports_mutex_);
            ports_.erase(device_number);
        }
    }
    bool acquire(const std::string& path, std::string& error)
    {
        fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { error = protocolError("打开串口失败（请检查路径和权限）", errno); return false; }
        struct stat info{};
        if (::fstat(fd, &info) != 0) { error = protocolError("查询串口设备失败", errno); return false; }
        if (!S_ISCHR(info.st_mode) || ::isatty(fd) != 1)
        { error = "RTU 路径不是有效的串口终端"; return false; }
        device_number = info.st_rdev;
        {
            std::lock_guard<std::mutex> lock(ports_mutex_);
            if (!ports_.insert(device_number).second)
            { error = "该串口已由另一个 RTU 连接使用"; return false; }
            claimed_ = true;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        { error = protocolError("串口已被占用或无法加锁", errno); return false; }
        int exclusive = 0;
        if (::ioctl(fd, TIOCGEXCL, &exclusive) != 0)
        { error = protocolError("无法查询串口独占状态", errno); return false; }
        if (exclusive) { error = "串口已被其他使用者设置为独占"; return false; }
        return true;
    }
    int fd{-1};
    dev_t device_number{};
private:
    bool claimed_{false};
    inline static std::mutex ports_mutex_;
    inline static std::unordered_set<dev_t> ports_;
};

struct RtuSession
{
    ~RtuSession()
    {
        // 在仍持有租约时恢复 termios、释放通信句柄，最后释放占用标记。
        if (restore_serial_mode && context && modbus_get_socket(context.get()) >= 0)
            ::ioctl(modbus_get_socket(context.get()), TIOCSRS485, &old_serial_mode);
        if (exclusive && context && modbus_get_socket(context.get()) >= 0)
            ::ioctl(modbus_get_socket(context.get()), TIOCNXCL);
        context.reset();
    }
    SerialLease lease;
    Context context;
    bool exclusive{false};
    serial_rs485 old_serial_mode{};
    bool restore_serial_mode{false};
};

std::uint32_t registerCount(ModbusDataType type)
{
    switch (type)
    {
        case ModbusDataType::Int16:
        case ModbusDataType::UInt16: return 1;
        case ModbusDataType::Int32:
        case ModbusDataType::UInt32:
        case ModbusDataType::Float32: return 2;
        case ModbusDataType::Int64:
        case ModbusDataType::UInt64:
        case ModbusDataType::Float64: return 4;
    }
    throw std::invalid_argument("未知 Modbus 数据类型");
}

// 完整的 Modbus 异常响应属于设备级失败，不等同于串口故障。
bool isDeviceException(int error) noexcept
{
    switch (error)
    {
        case EMBXILFUN:
        case EMBXILADD:
        case EMBXILVAL:
        case EMBXSFAIL:
        case EMBXACK:
        case EMBXSBUSY:
        case EMBXNACK:
        case EMBXMEMPAR:
        case EMBXGPATH:
        case EMBXGTAR:
            return true;
        default:
            return false;
    }
}

// 以二进制补码解释有符号数，避免超范围 unsigned -> signed 转换及有符号移位。
std::int64_t signedValue(std::uint64_t bits, unsigned width)
{
    const std::uint64_t sign = std::uint64_t{1} << (width - 1);
    if ((bits & sign) == 0) return static_cast<std::int64_t>(bits);
    const auto mask = width == 64 ? std::numeric_limits<std::uint64_t>::max()
                                 : (std::uint64_t{1} << width) - 1;
    return -1 - static_cast<std::int64_t>((~bits) & mask);
}

TelemetryValue decode(const ModbusRtuPointConfig& point, const std::uint16_t* registers)
{
    const auto count = registerCount(point.data_type);
    std::uint64_t bits = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto index = point.word_order == ModbusWordOrder::HighWordFirst ? i : count - 1 - i;
        std::uint16_t word = registers[index];
        // libmodbus 已将报文中的每个寄存器转换为主机 uint16_t。
        // 此处交换的是设备数值编码中的字节顺序，不重复处理网络字节序。
        if (point.byte_order == ModbusByteOrder::LowByteFirst)
            word = static_cast<std::uint16_t>((word >> 8) | (word << 8));
        bits = (bits << 16) | word;
    }

    TelemetryValue value;
    switch (point.data_type)
    {
        case ModbusDataType::Int16: value = signedValue(bits, 16); break;
        case ModbusDataType::Int32: value = signedValue(bits, 32); break;
        case ModbusDataType::Int64: value = signedValue(bits, 64); break;
        case ModbusDataType::UInt16:
        case ModbusDataType::UInt32:
        case ModbusDataType::UInt64: value = bits; break;
        case ModbusDataType::Float32:
        {
            static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
                          "Float32 decoding requires IEEE 754 binary32");
            const auto raw = static_cast<std::uint32_t>(bits);
            float number;
            std::memcpy(&number, &raw, sizeof(number));
            value = static_cast<double>(number);
            break;
        }
        case ModbusDataType::Float64:
        {
            static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559,
                          "Float64 decoding requires IEEE 754 binary64");
            double number;
            std::memcpy(&number, &bits, sizeof(number));
            value = number;
            break;
        }
        default: throw std::invalid_argument("未知 Modbus 数据类型");
    }

    if (const auto* number = std::get_if<double>(&value); number && !std::isfinite(*number))
        throw std::runtime_error("设备返回非有限浮点值");
    // 默认缩放不经过 double，保留 int64_t / uint64_t 的全部精度。
    if (point.scale == 1.0 && point.offset == 0.0) return value;

    double number;
    if (const auto* signed_number = std::get_if<std::int64_t>(&value))
        number = static_cast<double>(*signed_number);
    else if (const auto* unsigned_number = std::get_if<std::uint64_t>(&value))
        number = static_cast<double>(*unsigned_number);
    else
        number = std::get<double>(value);
    const double scaled = number * point.scale + point.offset;
    if (!std::isfinite(scaled)) throw std::runtime_error("工程值缩放结果不是有限数值");
    return scaled;
}

struct DeviceReadPlan
{
    const ModbusRtuDeviceConfig* device = nullptr;
    std::vector<const ModbusRtuPointConfig*> points;
    ModbusRegisterType type{ModbusRegisterType::Holding};
    std::uint32_t begin = 65536;
    std::uint32_t end = 0;
};

} // namespace

class ModbusRtuConnector::Impl
{
public:
    Impl(std::string connection_id, ModbusRtuConfig config, common::ILogger& logger)
        : connection_id_(std::move(connection_id)), config_(std::move(config)), logger_(logger)
    {
        if (connection_id_.find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
            connection_id_.find('\0') != std::string::npos)
            throw std::invalid_argument("connection_id 必须非空且不能包含空字符");
        std::string error;
        if (!ModbusRtuConfigParser{}.validate(config_, error)) throw std::invalid_argument(error);
        endpoint_key_ = makeEndpointKey(config_.device);
        response_timeout_ = toTimeout(config_.response_timeout);
        // 低波特率按实际串口帧长计算 3.5 字符；高波特率采用至少 1.75ms。
        const auto bits_per_character = 1 + config_.data_bits + (config_.parity == 'N' ? 0 : 1) + config_.stop_bits;
        const auto gap_us = config_.baud_rate > 19200 ? 1750LL :
            (3500000LL * bits_per_character + config_.baud_rate - 1) / config_.baud_rate;
        frame_gap_ = std::max(std::chrono::microseconds{gap_us},
            std::chrono::duration_cast<std::chrono::microseconds>(config_.inter_request_delay));
    }

    bool connect(std::string& error_message)
    {
        error_message.clear();
        if (connected_.load()) return true;
        disconnect();
        auto candidate = std::make_unique<RtuSession>();
        if (!candidate->lease.acquire(config_.device, error_message)) return false;
        // 通过已占用描述符的 /proc 路径打开同一终端，避免设备别名在两次 open
        // 之间改变而配置到另一串口。本驱动定位 Linux 本地串口，要求 /proc 可用。
        const auto held_path = "/proc/self/fd/" + std::to_string(candidate->lease.fd);
        candidate->context.reset(modbus_new_rtu(held_path.c_str(), config_.baud_rate,
            config_.parity, config_.data_bits, config_.stop_bits));
        if (!candidate->context)
        { error_message = protocolError("创建 Modbus RTU 上下文失败", errno); return false; }
        auto* ctx = candidate->context.get();
        if (modbus_set_error_recovery(ctx, MODBUS_ERROR_RECOVERY_NONE) == -1 ||
            !setTimeout(ctx, response_timeout_) || modbus_set_byte_timeout(ctx, 0, 0) == -1)
        { error_message = protocolError("设置 RTU 超时或恢复策略失败", errno); return false; }
        // 底层非阻塞 open；不做网络 connect，不配置没有实际意义的 TCP connect_timeout。
        if (modbus_connect(ctx) == -1)
        { error_message = protocolError("配置串口失败", errno); return false; }
        const int fd = modbus_get_socket(ctx);
        struct stat info{};
        if (::fstat(fd, &info) != 0)
        { error_message = protocolError("检查串口句柄失败", errno); return false; }
        if (info.st_rdev != candidate->lease.device_number)
        { error_message = "串口设备在打开期间发生变化"; return false; }
        // libmodbus 3.1.x 的接收等待使用 select，不能传入超出 fd_set 范围的句柄。
        if (fd < 0 || fd >= FD_SETSIZE)
        { error_message = "串口句柄超过 libmodbus select 支持范围"; return false; }
        if (::ioctl(fd, TIOCEXCL) != 0)
        { error_message = protocolError("设置串口独占失败", errno); return false; }
        candidate->exclusive = true;
        if (config_.serial_mode != "default")
        {
            // libmodbus 3.1.6 的新上下文默认缓存 RS232，直接设置 RS232 可能
            // 跳过内核操作。因此读取实际内核状态，仅切换 ENABLED 标志，保留
            // 已配置的方向极性和延时；会话结束时尽力恢复原模式。
            if (::ioctl(fd, TIOCGRS485, &candidate->old_serial_mode) != 0)
            { error_message = protocolError("查询串口模式失败（需内核驱动支持）", errno); return false; }
            auto requested_mode = candidate->old_serial_mode;
            if (config_.serial_mode == "rs485") requested_mode.flags |= SER_RS485_ENABLED;
            else requested_mode.flags &= ~SER_RS485_ENABLED;
            candidate->restore_serial_mode = true;
            if (::ioctl(fd, TIOCSRS485, &requested_mode) != 0)
            { error_message = protocolError("设置串口模式失败（需内核驱动支持）", errno); return false; }
            serial_rs485 actual_mode{};
            if (::ioctl(fd, TIOCGRS485, &actual_mode) != 0)
            { error_message = protocolError("确认串口模式失败", errno); return false; }
            if ((actual_mode.flags & SER_RS485_ENABLED) != (requested_mode.flags & SER_RS485_ENABLED))
            { error_message = "内核未应用要求的串口模式"; return false; }
        }
        // default 依赖转换器自动方向控制，不自动开启软件 RTS 或内核 RS485。
        if (modbus_flush(ctx) == -1)
        { error_message = protocolError("清理串口缓冲区失败", errno); return false; }
        session_ = std::move(candidate);
        needs_recovery_ = true; // 打开后也隔离前一个使用者可能留下的迟到响应。
        next_request_at_ = Clock::now();
        connected_.store(true);
        logConnected();
        return true;
    }

    void disconnect() noexcept
    {
        connected_.store(false);
        session_.reset();
        needs_recovery_ = false;
    }

    // RTU 没有 TCP 那样的独立连接会话。超时后不能只 flush 一次就立刻重发：
    // 隔离窗口内持续丢弃输入，窗口结束后还需一个完整帧间静默期。
    // 若总线持续有噪声，最多再等一个 response_timeout，然后关闭串口交给 Worker。
    bool recoverBus(std::string& error)
    {
        const int fd = modbus_get_socket(session_->context.get());
        const auto earliest = Clock::now() + config_.recovery_delay;
        const auto deadline = earliest + config_.response_timeout + frame_gap_;
        auto last_input = Clock::now();
        std::array<unsigned char, MODBUS_RTU_MAX_ADU_LENGTH> discarded{};
        for (;;)
        {
            const auto now = Clock::now();
            if (now >= deadline)
            { error = "RTU 总线在恢复窗口内未恢复静默"; return false; }
            const auto ready_at = std::max(earliest, last_input + frame_gap_);
            const auto remaining = std::min(deadline, ready_at) - now;
            // 到期时仍先 poll(0)，避免有积压输入时误判总线已经空闲。
            const int wait_ms = remaining <= Clock::duration::zero() ? 0 :
                static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
            pollfd event{fd, POLLIN, 0};
            const int rc = ::poll(&event, 1, wait_ms);
            if (rc < 0)
            {
                if (errno == EINTR) continue;
                error = protocolError("等待 RTU 总线恢复失败", errno); return false;
            }
            if (event.revents & (POLLERR | POLLHUP | POLLNVAL))
            { error = "RTU 串口已断开或发生 I/O 故障"; return false; }
            if (rc == 0)
            {
                if (Clock::now() >= ready_at) return true;
                continue;
            }
            const auto count = ::read(fd, discarded.data(), discarded.size());
            if (count > 0) { last_input = Clock::now(); continue; }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            error = count == 0 ? "RTU 串口在恢复期间关闭" : protocolError("丢弃 RTU 残留数据失败", errno);
            return false;
        }
    }

    bool prepareRequest(std::string& error)
    {
        if (needs_recovery_)
        {
            if (!recoverBus(error)) { disconnect(); return false; }
            needs_recovery_ = false;
        }
        else
        {
            // 正常请求之间也留出帧间隔；没有新增线程，等待仍在所属采集线程内。
            std::this_thread::sleep_until(next_request_at_);
        }
        return true;
    }

    // 在任何串口操作前检查完整任务；任务错误不能导致前半批设备已经被读取。
    bool makePlans(const CollectionTask& task, std::vector<DeviceReadPlan>& plans,
                   std::string& error_message) const
    {
        const auto fail = [&](const std::string& reason)
        {
            error_message = "连接 " + connection_id_ + " 的采集任务无效: " + reason;
            return false;
        };
        if (task.connection_id != connection_id_) return fail("connection_id 不匹配");
        if (task.interval.count() <= 0 || task.devices.empty()) return fail("周期必须大于 0，设备集合不能为空");
        std::size_t expected_devices = 0;
        for (const auto& device : config_.devices)
        {
            if (std::any_of(device.points.begin(), device.points.end(),
                [&](const auto& point) { return point.interval == task.interval; })) ++expected_devices;
        }
        if (task.devices.size() != expected_devices) return fail("任务必须包含该周期的全部设备");

        std::unordered_set<std::int64_t> device_ids;
        plans.reserve(task.devices.size());
        for (const auto& requested : task.devices)
        {
            const auto label = "device_id=" + std::to_string(requested.device_id) + ": ";
            if (!device_ids.insert(requested.device_id).second) return fail(label + "设备重复");
            const auto found = std::find_if(config_.devices.begin(), config_.devices.end(),
                [&](const auto& device) { return device.device_id == requested.device_id; });
            if (found == config_.devices.end()) return fail(label + "设备不存在");
            const auto expected_points = std::count_if(found->points.begin(), found->points.end(),
                [&](const auto& point) { return point.interval == task.interval; });
            if (requested.properties.empty() || requested.properties.size() != static_cast<std::size_t>(expected_points))
                return fail(label + "必须包含本设备该周期的全部点位");

            DeviceReadPlan plan;
            plan.device = &*found;
            plan.points.reserve(requested.properties.size());
            std::unordered_set<std::string> properties;
            for (const auto& property : requested.properties)
            {
                if (!properties.insert(property).second) return fail(label + "点位重复: " + property);
                const auto point = std::find_if(found->points.begin(), found->points.end(),
                    [&](const auto& item) { return item.property == property && item.interval == task.interval; });
                if (point == found->points.end()) return fail(label + "点位不存在或周期不匹配: " + property);
                if (plan.points.empty()) plan.type = point->register_type;
                else if (plan.type != point->register_type) return fail(label + "寄存器类型不一致");
                plan.begin = std::min(plan.begin, static_cast<std::uint32_t>(point->address));
                plan.end = std::max(plan.end, static_cast<std::uint32_t>(point->address) + registerCount(point->data_type));
                plan.points.push_back(&*point);
            }
            if (plan.end > 65536 || plan.end <= plan.begin || plan.end - plan.begin > MODBUS_MAX_READ_REGISTERS)
                return fail(label + "读取范围越界或超过单次请求数量限制");
            plans.push_back(std::move(plan));
        }
        return true;
    }

    CollectionResult readDevice(const DeviceReadPlan& plan)
    {
        CollectionResult result;
        result.device_id = plan.device->device_id;
        if (!connected_.load())
        {
            result.error_message = "Modbus RTU 串口未打开，等待上层重新打开";
            return result;
        }
        if (!prepareRequest(result.error_message)) return result;
        if (modbus_set_slave(session_->context.get(), plan.device->unit_id) == -1)
        {
            result.error_message = protocolError("设置 Unit ID 失败", errno);
            return result;
        }
        std::array<std::uint16_t, MODBUS_MAX_READ_REGISTERS> registers{};
        const int count = static_cast<int>(plan.end - plan.begin);
        const int received = plan.type == ModbusRegisterType::Holding
            ? modbus_read_registers(session_->context.get(), static_cast<int>(plan.begin), count, registers.data())
            : modbus_read_input_registers(session_->context.get(), static_cast<int>(plan.begin), count, registers.data());
        const int read_error = errno;
        next_request_at_ = Clock::now() + frame_gap_;
        if (received != count)
        {
            if (received < 0 && isDeviceException(read_error))
            {
                // 完整异常响应不破坏串口，后面的从站照常按顺序读取。
            }
            else if (received >= 0 || read_error == ETIMEDOUT || read_error == EINTR ||
                     read_error == EAGAIN || read_error == EMBBADCRC || read_error == EMBBADDATA ||
                     read_error == EMBBADEXC || read_error == EMBUNKEXC ||
                     read_error == EMBMDATA || read_error == EMBBADSLAVE)
            {
                needs_recovery_ = true;
            }
            else
            {
                // 例如 EIO/ENODEV/EBADF：串口资源不可用，本任务余下设备返回失败。
                disconnect();
            }
            result.error_message = received < 0 ? protocolError("设备寄存器读取失败", read_error)
                                               : "设备返回的寄存器数量不完整";
            return result;
        }

        std::vector<domain::model::TelemetryRecord> records;
        records.reserve(plan.points.size());
        for (const auto* point : plan.points)
        {
            try
            {
                const auto offset = static_cast<std::uint32_t>(point->address) - plan.begin;
                records.push_back({point->property, decode(*point, registers.data() + offset)});
            }
            catch (const std::runtime_error& error)
            {
                // 解码失败只影响本设备本轮结果，不提交已经解码的部分 records。
                result.error_message = "点位 " + point->property + " 解码失败: " + error.what();
                return result;
            }
        }
        result.records = std::move(records);
        result.status = CollectionStatus::Success;
        return result;
    }

    bool readPoints(const CollectionTask& task, std::vector<CollectionResult>& results,
                    std::string& error_message)
    {
        results.clear();
        error_message.clear();
        std::vector<DeviceReadPlan> plans;
        if (!makePlans(task, plans, error_message)) return false;
        std::vector<CollectionResult> completed;
        completed.reserve(plans.size());
        for (const auto& plan : plans) completed.push_back(readDevice(plan));
        results = std::move(completed);
        return true;
    }

    void logConnected() noexcept
    {
        try
        {
            logger_.info("ModbusRtuConnector", "连接已建立: " + connection_id_ + " (" +
                         config_.device + ")");
        }
        catch (...) {}
    }

    const std::string connection_id_;
    const std::string protocol_type_{"modbus_rtu"};
    const ModbusRtuConfig config_;
    std::string endpoint_key_;
    common::ILogger& logger_;
    Timeout response_timeout_{};
    using Clock = std::chrono::steady_clock;
    std::unique_ptr<RtuSession> session_;
    std::chrono::microseconds frame_gap_{};
    Clock::time_point next_request_at_{};
    bool needs_recovery_{false};
    std::atomic<bool> connected_{false};
};

ModbusRtuConnector::ModbusRtuConnector(std::string connection_id, ModbusRtuConfig config,
                                     common::ILogger& logger)
    : impl_(std::make_unique<Impl>(std::move(connection_id), std::move(config), logger))
{
}

ModbusRtuConnector::~ModbusRtuConnector()
{
    disconnect();
}

bool ModbusRtuConnector::connect(std::string& error_message)
{
    return impl_->connect(error_message);
}

void ModbusRtuConnector::disconnect() noexcept
{
    impl_->disconnect();
}

bool ModbusRtuConnector::isConnected() const noexcept
{
    return impl_->connected_.load();
}

bool ModbusRtuConnector::readPoints(const domain::model::CollectionTask& task,
                                  std::vector<domain::model::CollectionResult>& results,
                                  std::string& error_message)
{
    return impl_->readPoints(task, results, error_message);
}

const std::string& ModbusRtuConnector::connectionId() const noexcept
{
    return impl_->connection_id_;
}

const std::string& ModbusRtuConnector::protocolType() const noexcept
{
    return impl_->protocol_type_;
}

const std::string& ModbusRtuConnector::endpointKey() const noexcept
{
    return impl_->endpoint_key_;
}

} // namespace industrial_gateway::drivers::modbus_rtu
