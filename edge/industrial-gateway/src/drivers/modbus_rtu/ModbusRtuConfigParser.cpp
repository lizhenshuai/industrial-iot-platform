#include "ModbusRtuConfigParser.hpp"

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace industrial_gateway::drivers::modbus_rtu
{
namespace
{

using Json = nlohmann::json;

// 保持/输入寄存器单次读取上限。使用协议常量，解析器不依赖 libmodbus。
constexpr std::uint32_t kMaxReadRegisters = 125;
constexpr std::uint32_t kAddressSpaceSize = 65536;

// 仅用于内部展开解析失败；公共接口转换为 false 和带路径的错误信息。
class ConfigError final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void require(bool valid, const std::string& path, const std::string& reason)
{
    if (!valid) throw ConfigError(path + ": " + reason);
}

std::string elementPath(const std::string& parent, std::size_t index)
{
    return parent + "[" + std::to_string(index) + "]";
}

// 遍历原始映射检查重复键，再允许调用方按键读取。
void checkMap(const YAML::Node& node, const std::string& path,
              std::initializer_list<const char*> allowed)
{
    require(node.IsMap(), path, "必须是映射类型");
    std::unordered_set<std::string> seen;
    for (const auto& entry : node)
    {
        require(entry.first.IsScalar(), path, "配置项名称必须是标量");
        const auto key = entry.first.as<std::string>();
        const auto field = path + "." + key;
        require(seen.insert(key).second, field, "配置项重复");
        require(std::any_of(allowed.begin(), allowed.end(),
            [&](const char* name) { return key == name; }), field, "未知配置项");
    }
}

void checkMap(const Json& node, const std::string& path,
              std::initializer_list<const char*> allowed)
{
    require(node.is_object(), path, "必须是 JSON 对象");
    for (auto entry = node.begin(); entry != node.end(); ++entry)
    {
        require(std::any_of(allowed.begin(), allowed.end(),
            [&](const char* name) { return entry.key() == name; }),
            path + "." + entry.key(), "未知配置项");
    }
}

bool hasField(const YAML::Node& parent, const char* key)
{
    return parent[key].IsDefined();
}

bool hasField(const Json& parent, const char* key)
{
    return parent.find(key) != parent.end();
}

// 可选字段只有不存在时才使用默认值，显式 null 不属于缺省。
template<class T>
T readScalar(const YAML::Node& parent, const char* key, const std::string& path,
             bool required, T fallback)
{
    const auto field = path + "." + key;
    const auto node = parent[key];
    if (!node.IsDefined())
    {
        require(!required, field, "缺少必填配置项");
        return fallback;
    }
    require(node.IsScalar(), field, "必须是标量类型");
    try
    {
        return node.as<T>();
    }
    catch (const YAML::Exception& error)
    {
        throw ConfigError(field + ": 类型或格式错误: " + error.what());
    }
}

// JSON 读取不经过 YAML 标量转换，避免把 "1000"、true 或 1000.0
// 误当成整数，也避免 JSON 无符号数转 int64_t 时溢出回绕。
template<class T>
T readScalar(const Json& parent, const char* key, const std::string& path,
             bool required, T fallback)
{
    const auto field = path + "." + key;
    const auto found = parent.find(key);
    if (found == parent.end())
    {
        require(!required, field, "缺少必填配置项");
        return fallback;
    }
    const auto& node = *found;
    if constexpr (std::is_same_v<T, std::string>)
    {
        require(node.is_string(), field, "必须是字符串");
        return node.get<std::string>();
    }
    else if constexpr (std::is_same_v<T, std::int64_t>)
    {
        require(node.is_number_integer(), field, "必须是 JSON 整数");
        if (node.is_number_unsigned())
        {
            const auto value = node.get<std::uint64_t>();
            require(value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()),
                    field, "整数超过 int64_t 上限");
            return static_cast<std::int64_t>(value);
        }
        return node.get<std::int64_t>();
    }
    else
    {
        static_assert(std::is_same_v<T, double>, "不支持的 JSON 配置标量类型");
        require(node.is_number(), field, "必须是 JSON 数值");
        const auto value = node.get<double>();
        require(std::isfinite(value), field, "必须是有限数值");
        return value;
    }
}

template<class Node>
std::int64_t readInteger(const Node& parent, const char* key,
                         const std::string& path, bool required,
                         std::int64_t fallback, std::int64_t minimum,
                         std::int64_t maximum)
{
    const auto value = readScalar<std::int64_t>(parent, key, path, required, fallback);
    require(value >= minimum && value <= maximum, path + "." + key,
            "必须在 " + std::to_string(minimum) + " 到 " + std::to_string(maximum) + " 之间");
    return value;
}

template<class Node>
std::chrono::milliseconds readDuration(const Node& parent, const char* key,
                                       const std::string& path,
                                       std::chrono::milliseconds fallback)
{
    using Rep = std::chrono::milliseconds::rep;
    const auto value = readInteger(parent, key, path, false, fallback.count(), 1,
                                   std::numeric_limits<Rep>::max());
    return std::chrono::milliseconds{static_cast<Rep>(value)};
}

template<class Enum, class Node>
Enum readEnum(const Node& parent, const char* key, const std::string& path,
              Enum fallback, std::initializer_list<std::pair<const char*, Enum>> choices)
{
    if (!hasField(parent, key)) return fallback;
    const auto value = readScalar<std::string>(parent, key, path, true, {});
    std::string supported;
    for (const auto& choice : choices)
    {
        if (value == choice.first) return choice.second;
        if (!supported.empty()) supported += " / ";
        supported += choice.first;
    }
    throw ConfigError(path + "." + key + ": 不支持的值 '" + value + "'，允许值: " + supported);
}

void checkText(const std::string& value, const std::string& path)
{
    require(value.find_first_not_of(" \t\r\n\f\v") != std::string::npos,
            path, "不能是空字符串或全空白字符串");
    require(value.find('\0') == std::string::npos, path, "不能包含空字符");
}

std::uint32_t registerCount(ModbusDataType type, const std::string& path)
{
    switch (type)
    {
        case ModbusDataType::Int16:
        case ModbusDataType::UInt16:
            return 1;
        case ModbusDataType::Int32:
        case ModbusDataType::UInt32:
        case ModbusDataType::Float32:
            return 2;
        case ModbusDataType::Int64:
        case ModbusDataType::UInt64:
        case ModbusDataType::Float64:
            return 4;
    }
    throw ConfigError(path + ": 未知数据类型");
}

template<class Node>
ModbusRtuPointConfig parsePoint(const Node& node, const std::string& path)
{
    checkMap(node, path, {"property", "register_type", "address", "data_type",
                         "byte_order", "word_order", "interval_ms", "scale", "offset"});
    ModbusRtuPointConfig point;
    point.property = readScalar<std::string>(node, "property", path, true, {});
    point.address = static_cast<std::uint16_t>(readInteger(
        node, "address", path, true, 0, 0, kAddressSpaceSize - 1));
    point.register_type = readEnum<ModbusRegisterType>(node, "register_type", path,
        point.register_type, {{"holding", ModbusRegisterType::Holding},
                              {"input", ModbusRegisterType::Input}});
    point.data_type = readEnum<ModbusDataType>(node, "data_type", path, point.data_type,
        {{"int16", ModbusDataType::Int16}, {"uint16", ModbusDataType::UInt16},
         {"int32", ModbusDataType::Int32}, {"uint32", ModbusDataType::UInt32},
         {"int64", ModbusDataType::Int64}, {"uint64", ModbusDataType::UInt64},
         {"float32", ModbusDataType::Float32}, {"float64", ModbusDataType::Float64}});
    point.byte_order = readEnum<ModbusByteOrder>(node, "byte_order", path, point.byte_order,
        {{"high_byte_first", ModbusByteOrder::HighByteFirst},
         {"low_byte_first", ModbusByteOrder::LowByteFirst}});
    point.word_order = readEnum<ModbusWordOrder>(node, "word_order", path, point.word_order,
        {{"high_word_first", ModbusWordOrder::HighWordFirst},
         {"low_word_first", ModbusWordOrder::LowWordFirst}});
    point.interval = readDuration(node, "interval_ms", path, point.interval);
    point.scale = readScalar<double>(node, "scale", path, false, point.scale);
    point.offset = readScalar<double>(node, "offset", path, false, point.offset);
    return point;
}

// 两种输入共用串口字段读取；host、port 等 TCP 字段不会被接受。
template<class Node>
void parseConnection(const Node& node, ModbusRtuConfig& config)
{
    checkMap(node, "connection", {"device", "baud_rate", "parity", "data_bits", "stop_bits",
        "serial_mode", "response_timeout_ms", "recovery_delay_ms", "inter_request_delay_ms"});
    config.device = readScalar<std::string>(node, "device", "connection", true, {});
    config.baud_rate = static_cast<int>(readInteger(node, "baud_rate", "connection", false,
        config.baud_rate, 1, std::numeric_limits<int>::max()));
    const auto parity = readScalar<std::string>(node, "parity", "connection", false, "N");
    require(parity == "N" || parity == "E" || parity == "O", "connection.parity", "必须为 N / E / O");
    config.parity = parity.front();
    config.data_bits = static_cast<int>(readInteger(node, "data_bits", "connection", false,
        config.data_bits, 8, 8));
    config.stop_bits = static_cast<int>(readInteger(node, "stop_bits", "connection", false,
        config.stop_bits, 1, 2));
    config.serial_mode = readScalar<std::string>(node, "serial_mode", "connection", false, config.serial_mode);
    config.response_timeout = std::chrono::milliseconds{readInteger(node, "response_timeout_ms", "connection",
        false, config.response_timeout.count(), 1, 60000)};
    config.recovery_delay = std::chrono::milliseconds{readInteger(node, "recovery_delay_ms", "connection",
        false, config.recovery_delay.count(), 1, 60000)};
    config.inter_request_delay = std::chrono::milliseconds{readInteger(node, "inter_request_delay_ms", "connection",
        false, config.inter_request_delay.count(), 0, 1000)};
}

// 即使直接传入协议 JSON，也必须在库覆盖同名成员之前拒绝重复键。
Json parseJsonDocument(const std::string& text)
{
    require(text.find('\0') == std::string::npos,
            "root", "JSON 配置不能包含未转义的空字符");
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed)
    {
        switch (event)
        {
            case Json::parse_event_t::object_start:
                object_keys.emplace_back();
                break;
            case Json::parse_event_t::key:
            {
                const auto& key = parsed.get_ref<const std::string&>();
                require(object_keys.back().insert(key).second,
                        "root", "JSON 对象中存在重复配置项 " + parsed.dump());
                break;
            }
            case Json::parse_event_t::object_end:
                object_keys.pop_back();
                break;
            default:
                break;
        }
        return true;
    };
    return Json::parse(text.begin(), text.end(), callback, true, false);
}

const Json& requiredNode(const Json& parent, const char* key, const std::string& path)
{
    const auto found = parent.find(key);
    require(found != parent.end(), path, "缺少必填配置项");
    return *found;
}

// end 是排他边界，使用宽整数，允许最后一个寄存器的边界等于 65536。
struct ReadRange
{
    ModbusRegisterType type;
    std::uint32_t begin;
    std::uint32_t end;
};

} // namespace


// ==================================================
// 解析 YAML：只有整份配置合法才提交输出
// ==================================================

bool ModbusRtuConfigParser::parse(
    const YAML::Node& root,
    ModbusRtuConfig& config,
    std::string& error_message
) const
{
    error_message.clear();
    try
    {
        checkMap(root, "root", {"connection", "devices"});
        ModbusRtuConfig candidate;
        const auto connection = root["connection"];
        parseConnection(connection, candidate);

        const auto devices = root["devices"];
        require(devices.IsSequence(), "devices", "必须是设备序列");
        candidate.devices.reserve(devices.size());
        for (std::size_t i = 0; i < devices.size(); ++i)
        {
            const auto path = elementPath("devices", i);
            const auto node = devices[i];
            checkMap(node, path, {"device_id", "unit_id", "points"});
            ModbusRtuDeviceConfig device;
            device.device_id = readScalar<std::int64_t>(node, "device_id", path, true, 0);
            device.unit_id = static_cast<int>(readInteger(
                node, "unit_id", path, false, device.unit_id, 1, 247));
            const auto points = node["points"];
            require(points.IsSequence(), path + ".points", "必须是点位序列");
            device.points.reserve(points.size());
            for (std::size_t j = 0; j < points.size(); ++j)
                device.points.push_back(parsePoint(points[j], elementPath(path + ".points", j)));
            candidate.devices.push_back(std::move(device));
        }

        if (!validate(candidate, error_message)) return false;
        config = std::move(candidate);
        return true;
    }
    catch (const ConfigError& error)
    {
        error_message = error.what();
    }
    catch (const YAML::Exception& error)
    {
        error_message = std::string("Modbus RTU YAML 配置解析失败: ") + error.what();
    }
    return false;
}


// ==================================================
// 解析 JSON：字段转换保持严格类型，最终复用同一份语义校验
// ==================================================

bool ModbusRtuConfigParser::parseJson(
    const std::string& config_json,
    ModbusRtuConfig& config,
    std::string& error_message
) const
{
    error_message.clear();
    try
    {
        const auto root = parseJsonDocument(config_json);
        checkMap(root, "root", {"connection", "devices"});
        ModbusRtuConfig candidate;
        const auto& connection = requiredNode(root, "connection", "connection");
        parseConnection(connection, candidate);

        const auto& devices = requiredNode(root, "devices", "devices");
        require(devices.is_array(), "devices", "必须是 JSON 数组");
        candidate.devices.reserve(devices.size());
        for (std::size_t i = 0; i < devices.size(); ++i)
        {
            const auto path = elementPath("devices", i);
            const auto& node = devices[i];
            checkMap(node, path, {"device_id", "unit_id", "points"});
            ModbusRtuDeviceConfig device;
            // 沿用当前模型的 int64_t 设备 ID 范围，不擅自增加正数约束。
            device.device_id = readScalar<std::int64_t>(node, "device_id", path, true, 0);
            device.unit_id = static_cast<int>(readInteger(
                node, "unit_id", path, false, device.unit_id, 1, 247));
            const auto& points = requiredNode(node, "points", path + ".points");
            require(points.is_array(), path + ".points", "必须是 JSON 数组");
            device.points.reserve(points.size());
            for (std::size_t j = 0; j < points.size(); ++j)
                device.points.push_back(parsePoint(points[j], elementPath(path + ".points", j)));
            candidate.devices.push_back(std::move(device));
        }

        // 设备、点位唯一性及同频率连续读取范围均由已有 validate() 检查。
        if (!validate(candidate, error_message)) return false;
        config = std::move(candidate);
        return true;
    }
    catch (const ConfigError& error)
    {
        error_message = error.what();
    }
    catch (const Json::parse_error& error)
    {
        error_message = "Modbus RTU JSON 语法错误，字节位置 " + std::to_string(error.byte);
    }
    catch (const Json::out_of_range&)
    {
        error_message = "Modbus RTU JSON 数值或容器大小超出解析器支持的范围";
    }
    catch (const Json::exception&)
    {
        // 不回显 JSON 库可能携带的配置原文。
        error_message = "Modbus RTU JSON 配置解析失败";
    }
    return false;
}


// ==================================================
// 配置语义与请求范围校验：也适用于直接构造的 C++ 配置
// ==================================================

bool ModbusRtuConfigParser::validate(
    const ModbusRtuConfig& config,
    std::string& error_message
) const
{
    error_message.clear();
    try
    {
        checkText(config.device, "connection.device");
        require(std::filesystem::path(config.device).is_absolute(), "connection.device", "必须是本机串口绝对路径");
        // 第一版只接受 Linux/libmodbus 常见波特率，避免库对不支持的速率静默使用默认值。
        constexpr int baud_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
        bool supported = false;
        for (const int rate : baud_rates) if (rate == config.baud_rate) supported = true;
        require(supported, "connection.baud_rate", "支持 1200/2400/4800/9600/19200/38400/57600/115200");
        require(config.parity == 'N' || config.parity == 'E' || config.parity == 'O',
            "connection.parity", "必须为 N / E / O");
        require(config.data_bits == 8, "connection.data_bits", "RTU 数据位必须为 8");
        require(config.stop_bits == 1 || config.stop_bits == 2, "connection.stop_bits", "必须为 1 或 2");
        require(config.serial_mode == "default" || config.serial_mode == "rs485" || config.serial_mode == "rs232",
            "connection.serial_mode", "必须为 default / rs485 / rs232");
        require(config.response_timeout.count() >= 1 && config.response_timeout.count() <= 60000,
            "connection.response_timeout_ms", "必须在 1..60000ms 之间");
        require(config.recovery_delay.count() >= 1 && config.recovery_delay.count() <= 60000,
            "connection.recovery_delay_ms", "必须在 1..60000ms 之间");
        require(config.inter_request_delay.count() >= 0 && config.inter_request_delay.count() <= 1000,
            "connection.inter_request_delay_ms", "必须在 0..1000ms 之间");
        require(!config.devices.empty(), "devices", "至少需要一个设备");

        std::unordered_set<std::int64_t> device_ids;
        std::unordered_set<int> unit_ids;
        for (std::size_t i = 0; i < config.devices.size(); ++i)
        {
            const auto& device = config.devices[i];
            const auto path = elementPath("devices", i);
            require(device_ids.insert(device.device_id).second, path + ".device_id",
                    "设备 ID 重复: " + std::to_string(device.device_id));
            // 采集不发送广播，RTU 也不接受 TCP 的特殊地址 255。
            require(device.unit_id >= 1 && device.unit_id <= 247,
                    path + ".unit_id", "必须在 1 到 247 之间");
            require(unit_ids.insert(device.unit_id).second, path + ".unit_id",
                    "同串口从站地址重复，请将点位配置放在同一设备中");
            require(!device.points.empty(), path + ".points", "至少需要一个点位");

            std::unordered_set<std::string> properties;
            std::map<std::chrono::milliseconds::rep, ReadRange> ranges;
            for (std::size_t j = 0; j < device.points.size(); ++j)
            {
                const auto& point = device.points[j];
                const auto point_path = elementPath(path + ".points", j);
                checkText(point.property, point_path + ".property");
                require(properties.insert(point.property).second, point_path + ".property",
                        "同设备点位名称重复: " + point.property);
                require(point.register_type == ModbusRegisterType::Holding ||
                        point.register_type == ModbusRegisterType::Input,
                        point_path + ".register_type", "未知寄存器类型");
                require(point.byte_order == ModbusByteOrder::HighByteFirst ||
                        point.byte_order == ModbusByteOrder::LowByteFirst,
                        point_path + ".byte_order", "未知字节顺序");
                require(point.word_order == ModbusWordOrder::HighWordFirst ||
                        point.word_order == ModbusWordOrder::LowWordFirst,
                        point_path + ".word_order", "未知寄存器顺序");
                require(point.interval.count() > 0, point_path + ".interval_ms", "必须大于 0");
                require(std::isfinite(point.scale), point_path + ".scale", "必须是有限数值");
                require(std::isfinite(point.offset), point_path + ".offset", "必须是有限数值");

                const std::uint32_t begin = point.address;
                const std::uint32_t end = begin + registerCount(point.data_type, point_path + ".data_type");
                require(end <= kAddressSpaceSize, point_path + ".address",
                        "点位占用的寄存器范围超过地址上限 65535");

                auto entry = ranges.emplace(point.interval.count(), ReadRange{point.register_type, begin, end});
                auto& range = entry.first->second;
                require(range.type == point.register_type, point_path + ".register_type",
                        "同设备、同周期的点位必须使用相同寄存器类型，才能通过一次请求读取");
                range.begin = std::min(range.begin, begin);
                range.end = std::max(range.end, end);
                // 校验连续覆盖范围而非点位数量之和；间隙也占用请求数量。
                require(range.end - range.begin <= kMaxReadRegisters, point_path + ".address",
                        "设备 " + std::to_string(device.device_id) + "、周期 " +
                        std::to_string(point.interval.count()) + "ms 的连续读取范围超过 125 个寄存器");
            }
        }
        return true;
    }
    catch (const ConfigError& error)
    {
        error_message = error.what();
        return false;
    }
}

} // namespace industrial_gateway::drivers::modbus_rtu
