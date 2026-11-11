#include "ModbusTcpConnector.hpp"
#include "ModbusTcpConfigParser.hpp"

#include <modbus/modbus.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace industrial_gateway::drivers::modbus_tcp
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

// 明确的设备异常响应已经构成完整报文，不等同于 TCP 流损坏。
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

TelemetryValue decode(const ModbusTcpPointConfig& point, const std::uint16_t* registers)
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
    const ModbusTcpDeviceConfig* device = nullptr;
    std::vector<const ModbusTcpPointConfig*> points;
    ModbusRegisterType type{ModbusRegisterType::Holding};
    std::uint32_t begin = 65536;
    std::uint32_t end = 0;
};

} // namespace

class ModbusTcpConnector::Impl
{
public:
    Impl(std::string connection_id, ModbusTcpConfig config, common::ILogger& logger)
        : connection_id_(std::move(connection_id)), config_(std::move(config)), logger_(logger)
    {
        if (connection_id_.find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
            connection_id_.find('\0') != std::string::npos)
            throw std::invalid_argument("connection_id 必须非空且不能包含空字符");
        std::string error;
        if (!ModbusTcpConfigParser{}.validate(config_, error)) throw std::invalid_argument(error);
        in_addr address{};
        if (::inet_pton(AF_INET, config_.host.c_str(), &address) != 1)
            throw std::invalid_argument("第一版 Modbus TCP host 必须是数字 IPv4 地址，不支持域名解析");
        // inet_pton 已限定为标准数字 IPv4；同端点的不同设备共用这一个连接。
        endpoint_key_ = config_.host + ":" + std::to_string(config_.port);
        connect_timeout_ = toTimeout(config_.connect_timeout);
        response_timeout_ = toTimeout(config_.response_timeout);
    }

    bool connect(std::string& error_message)
    {
        error_message.clear();
        if (connected_.load()) return true;
        disconnect();
        Context candidate(modbus_new_tcp(config_.host.c_str(), config_.port));
        if (!candidate)
        {
            error_message = protocolError("创建 Modbus TCP 上下文失败", errno);
            return false;
        }
        // 当前 libmodbus 3.1.6 TCP 后端使用 response_timeout 限制连接等待。
        // 连接前设置连接超时，连接后改回读取响应超时。
        // 源码依据：https://github.com/stephane/libmodbus/blob/v3.1.6/src/modbus-tcp.c
        if (modbus_set_error_recovery(candidate.get(), MODBUS_ERROR_RECOVERY_NONE) == -1 ||
            !setTimeout(candidate.get(), connect_timeout_) ||
            modbus_set_byte_timeout(candidate.get(), 0, 0) == -1)
        {
            error_message = protocolError("设置 Modbus 超时或恢复策略失败", errno);
            return false;
        }
        if (modbus_connect(candidate.get()) == -1)
        {
            error_message = protocolError("Modbus TCP 连接失败", errno);
            return false;
        }
        // 关闭字节间超时，让 response_timeout 约束完整响应接收。
        // https://libmodbus.org/reference/modbus_set_byte_timeout/
        if (!setTimeout(candidate.get(), response_timeout_))
        {
            error_message = protocolError("设置读取响应超时失败", errno);
            return false;
        }
        context_ = std::move(candidate);
        connected_.store(true);
        logConnected();
        return true;
    }

    void disconnect() noexcept
    {
        connected_.store(false);
        context_.reset();
    }

    // 在任何网络操作前检查完整任务；任务错误不能导致前半批设备已经被读取。
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
            result.error_message = "Modbus TCP 未连接，等待上层重新建立连接";
            return result;
        }
        if (modbus_set_slave(context_.get(), plan.device->unit_id) == -1)
        {
            result.error_message = protocolError("设置 Unit ID 失败", errno);
            return result;
        }
        std::array<std::uint16_t, MODBUS_MAX_READ_REGISTERS> registers{};
        const int count = static_cast<int>(plan.end - plan.begin);
        const int received = plan.type == ModbusRegisterType::Holding
            ? modbus_read_registers(context_.get(), static_cast<int>(plan.begin), count, registers.data())
            : modbus_read_input_registers(context_.get(), static_cast<int>(plan.begin), count, registers.data());
        const int read_error = errno;
        if (received != count)
        {
            // 超时、网络错误或损坏响应后弃用此连接，不让迟到响应进入下一次请求。
            // 已返回完整异常响应时保留连接，继续处理其他设备。
            if (received >= 0 || !isDeviceException(read_error)) disconnect();
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
            logger_.info("ModbusTcpConnector", "连接已建立: " + connection_id_ + " (" +
                         config_.host + ":" + std::to_string(config_.port) + ")");
        }
        catch (...) {}
    }

    const std::string connection_id_;
    const std::string protocol_type_{"modbus_tcp"};
    const ModbusTcpConfig config_;
    std::string endpoint_key_;
    common::ILogger& logger_;
    Timeout connect_timeout_{};
    Timeout response_timeout_{};
    Context context_;
    std::atomic<bool> connected_{false};
};

ModbusTcpConnector::ModbusTcpConnector(std::string connection_id, ModbusTcpConfig config,
                                     common::ILogger& logger)
    : impl_(std::make_unique<Impl>(std::move(connection_id), std::move(config), logger))
{
}

ModbusTcpConnector::~ModbusTcpConnector()
{
    disconnect();
}

bool ModbusTcpConnector::connect(std::string& error_message)
{
    return impl_->connect(error_message);
}

void ModbusTcpConnector::disconnect() noexcept
{
    impl_->disconnect();
}

bool ModbusTcpConnector::isConnected() const noexcept
{
    return impl_->connected_.load();
}

bool ModbusTcpConnector::readPoints(const domain::model::CollectionTask& task,
                                  std::vector<domain::model::CollectionResult>& results,
                                  std::string& error_message)
{
    return impl_->readPoints(task, results, error_message);
}

const std::string& ModbusTcpConnector::connectionId() const noexcept
{
    return impl_->connection_id_;
}

const std::string& ModbusTcpConnector::protocolType() const noexcept
{
    return impl_->protocol_type_;
}

const std::string& ModbusTcpConnector::endpointKey() const noexcept
{
    return impl_->endpoint_key_;
}

} // namespace industrial_gateway::drivers::modbus_tcp
