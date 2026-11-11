#include "OpcUaConfigParser.hpp"
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <open62541/types.h>
#include <open62541/types_generated.h>
#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>

namespace industrial_gateway::drivers::opcua
{
namespace
{
using Json = nlohmann::json;
// 保持项目现有的严格 JSON/YAML 标量读取规则，不经字符串中转 JSON 数值。
class ConfigError final : public std::invalid_argument
{
public:
    using std::invalid_argument::invalid_argument;
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

void checkText(const std::string& value, const std::string& path)
{
    require(value.find_first_not_of(" \t\r\n\f\v") != std::string::npos &&
            value.find('\0') == std::string::npos, path, "不能为空白或包含空字符");
    // YAML 字符串也必须能在现有 JSON 遥测链路中表示。
    (void)Json(value).dump();
}

std::uint32_t decimal(std::string_view value, std::uint32_t maximum, const std::string& path)
{
    require(!value.empty() && std::all_of(value.begin(), value.end(),
        [](char ch) { return ch >= '0' && ch <= '9'; }), path, "必须是无符号十进制数字");
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result <= maximum,
            path, "数字超出允许范围");
    return result;
}

void checkNodeId(const std::string& text, const std::string& path)
{
    checkText(text, path);
    require(text.size() <= 4096, path, "NodeId 文本不能超过 4096 字节");
    std::string_view remaining(text);
    if (remaining.substr(0, 3) == "ns=")
    {
        const auto separator = remaining.find(';');
        require(separator != std::string_view::npos, path, "缺少命名空间分隔符");
        (void)decimal(remaining.substr(3, separator - 3), 65535, path);
        remaining.remove_prefix(separator + 1);
    }
    if (remaining.substr(0, 2) == "i=")
        (void)decimal(remaining.substr(2), std::numeric_limits<std::uint32_t>::max(), path);
    else
        require(remaining.substr(0, 2) == "s=" && remaining.size() > 2,
                path, "仅支持 [ns=N;]i=数字 或 [ns=N;]s=非空字符串");
    UA_NodeId node{};
    const auto status = UA_NodeId_parse(&node, UA_STRING(const_cast<char*>(text.c_str())));
    const bool valid = UA_StatusCode_isGood(status) && !UA_NodeId_isNull(&node) &&
        (node.identifierType == UA_NODEIDTYPE_NUMERIC || node.identifierType == UA_NODEIDTYPE_STRING);
    UA_NodeId_clear(&node);
    require(valid, path, "NodeId 无效或为 Null NodeId");
}

YAML::Node child(const YAML::Node& parent, const char* key, const std::string& path)
{
    const auto value = parent[key];
    require(value.IsDefined(), path, "缺少必填配置项");
    return value;
}
const Json& child(const Json& parent, const char* key, const std::string& path)
{
    const auto value = parent.find(key);
    require(value != parent.end(), path, "缺少必填配置项");
    return *value;
}
void sequence(const YAML::Node& node, const std::string& path)
{ require(node.IsSequence(), path, "必须是序列"); }
void sequence(const Json& node, const std::string& path)
{ require(node.is_array(), path, "必须是 JSON 数组"); }

template<class Node>
OpcUaConfig parseModel(const Node& root)
{
    checkMap(root, "root", {"connection", "devices"});
    OpcUaConfig config;
    const auto& connection = child(root, "connection", "connection");
    checkMap(connection, "connection", {"endpoint_url", "timeout_ms", "security_mode", "authentication"});
    config.endpoint_url = readScalar<std::string>(connection, "endpoint_url", "connection", true, {});
    config.timeout = std::chrono::milliseconds{readInteger(connection, "timeout_ms", "connection",
        false, config.timeout.count(), 1, 60000)};
    config.security_mode = readScalar<std::string>(connection, "security_mode", "connection", false, config.security_mode);
    config.authentication = readScalar<std::string>(connection, "authentication", "connection", false, config.authentication);
    const auto& devices = child(root, "devices", "devices");
    sequence(devices, "devices");
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        const auto path = elementPath("devices", i);
        const auto node = devices[i];
        checkMap(node, path, {"device_id", "points"});
        OpcUaDeviceConfig device;
        device.device_id = readScalar<std::int64_t>(node, "device_id", path, true, 0);
        const auto& points = child(node, "points", path + ".points");
        sequence(points, path + ".points");
        for (std::size_t j = 0; j < points.size(); ++j)
        {
            const auto label = elementPath(path + ".points", j);
            const auto point_node = points[j];
            checkMap(point_node, label, {"property", "node_id", "interval_ms", "scale", "offset"});
            OpcUaPointConfig point;
            point.property = readScalar<std::string>(point_node, "property", label, true, {});
            point.node_id = readScalar<std::string>(point_node, "node_id", label, true, {});
            point.interval = std::chrono::milliseconds{readInteger(point_node, "interval_ms", label,
                false, 1000, 1, std::numeric_limits<std::chrono::milliseconds::rep>::max())};
            point.scale = readScalar<double>(point_node, "scale", label, false, point.scale);
            point.offset = readScalar<double>(point_node, "offset", label, false, point.offset);
            device.points.push_back(std::move(point));
        }
        config.devices.push_back(std::move(device));
    }
    return config;
}
} // namespace

std::string OpcUaConfigParser::normalizeEndpoint(const std::string& url)
{
    const std::string label = "connection.endpoint_url";
    checkText(url, label);
    require(url.size() <= 4096 && url.compare(0, 10, "opc.tcp://") == 0, label,
            "必须是 opc.tcp:// 地址，且不超过 4096 字节");
    require(std::all_of(url.begin(), url.end(), [](unsigned char ch) { return ch > 32 && ch < 127; }) &&
            url.find_first_of("?#\\") == std::string::npos, label, "不支持空白、非 ASCII、查询、片段或反斜杠");
    const auto slash = url.find('/', 10);
    const auto authority = url.substr(10, slash == std::string::npos ? std::string::npos : slash - 10);
    auto path = slash == std::string::npos ? std::string{} : url.substr(slash);
    if (path == "/") path.clear();
    require(!authority.empty() && authority.find('@') == std::string::npos, label, "主机不能为空或包含用户信息");
    std::string host, port_text;
    bool has_port = false;
    char buffer[INET6_ADDRSTRLEN]{};
    if (authority.front() == '[')
    {
        const auto end = authority.find(']');
        require(end != std::string::npos, label, "IPv6 地址缺少右方括号");
        const auto address = authority.substr(1, end - 1);
        in6_addr parsed{};
        require(::inet_pton(AF_INET6, address.c_str(), &parsed) == 1, label, "无效 IPv6 地址");
        require(::inet_ntop(AF_INET6, &parsed, buffer, sizeof(buffer)) != nullptr, label, "IPv6 地址转换失败");
        host = "[" + std::string(buffer) + "]";
        if (end + 1 < authority.size())
        {
            require(authority[end + 1] == ':', label, "IPv6 地址后仅允许端口");
            has_port = true;
            port_text = authority.substr(end + 2);
        }
    }
    else
    {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        has_port = colon != std::string::npos;
        if (has_port) port_text = authority.substr(colon + 1);
        require(!host.empty(), label, "主机不能为空");
        in_addr parsed{};
        if (::inet_pton(AF_INET, host.c_str(), &parsed) == 1)
        {
            require(::inet_ntop(AF_INET, &parsed, buffer, sizeof(buffer)) != nullptr, label, "IPv4 地址转换失败");
            host = buffer;
        }
        else
        {
            // 不接受 127.1 等依赖系统特殊解释的数字地址。
            require(!std::all_of(host.begin(), host.end(), [](char ch) {
                return (ch >= '0' && ch <= '9') || ch == '.'; }), label, "无效数字 IPv4 地址");
            if (host.back() == '.') host.pop_back();
            require(!host.empty() && host.back() != '.' && host.size() <= 253, label, "域名长度或结尾无效");
            std::transform(host.begin(), host.end(), host.begin(), [](char ch) {
                return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
            });
            std::size_t start = 0;
            while (start < host.size())
            {
                const auto end = host.find('.', start);
                const auto part = host.substr(start, end == std::string::npos ? end : end - start);
                require(!part.empty() && part.size() <= 63 && part.front() != '-' && part.back() != '-' &&
                    std::all_of(part.begin(), part.end(), [](char ch) {
                        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
                    }), label, "无效域名标签");
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
    }
    const auto port = has_port ? decimal(port_text, 65535, label) : 4840;
    require(port > 0, label, "端口必须为 1..65535");
    return "opc.tcp://" + host + ":" + std::to_string(port) + path;
}

bool OpcUaConfigParser::validate(const OpcUaConfig& config, std::string& error) const
{
    error.clear();
    try
    {
        (void)normalizeEndpoint(config.endpoint_url);
        require(config.timeout.count() >= 1 && config.timeout.count() <= 60000, "connection.timeout_ms", "必须在 1..60000");
        require(config.security_mode == "None", "connection.security_mode", "第一版仅支持 None");
        require(config.authentication == "anonymous", "connection.authentication", "第一版仅支持 anonymous");
        require(!config.devices.empty(), "devices", "不能为空");
        std::unordered_set<std::int64_t> ids;
        for (std::size_t i = 0; i < config.devices.size(); ++i)
        {
            const auto& device = config.devices[i];
            const auto path = elementPath("devices", i);
            require(ids.insert(device.device_id).second, path, "device_id 重复");
            require(!device.points.empty(), path + ".points", "不能为空");
            std::unordered_set<std::string> properties;
            std::map<std::chrono::milliseconds::rep, std::size_t> counts;
            for (std::size_t j = 0; j < device.points.size(); ++j)
            {
                const auto& point = device.points[j];
                const auto label = elementPath(path + ".points", j);
                checkText(point.property, label + ".property");
                require(properties.insert(point.property).second, label, "property 重复");
                checkNodeId(point.node_id, label + ".node_id");
                require(point.interval.count() > 0, label + ".interval_ms", "必须大于 0");
                require(std::isfinite(point.scale) && std::isfinite(point.offset), label, "缩放系数必须是有限数值");
                // 这是本驱动的资源上限，不是 OPC UA 标准规定的上限。
                require(++counts[point.interval.count()] <= 1024, label, "同设备同周期最多 1024 个点位");
            }
        }
        return true;
    }
    catch (const std::invalid_argument& exception) { error = exception.what(); }
    catch (const Json::exception&) { error = "OPC UA 配置字符串不是有效 UTF-8"; }
    return false;
}

bool OpcUaConfigParser::parse(const YAML::Node& root, OpcUaConfig& config, std::string& error) const
{
    error.clear();
    try
    {
        auto candidate = parseModel(root);
        if (!validate(candidate, error)) return false;
        config = std::move(candidate);
        return true;
    }
    catch (const std::invalid_argument& exception) { error = exception.what(); }
    catch (const YAML::Exception& exception) { error = std::string("OPC UA YAML 解析失败: ") + exception.what(); }
    return false;
}

bool OpcUaConfigParser::parseJson(const std::string& text, OpcUaConfig& config, std::string& error) const
{
    error.clear();
    try
    {
        auto candidate = parseModel(parseJsonDocument(text));
        if (!validate(candidate, error)) return false;
        config = std::move(candidate);
        return true;
    }
    catch (const std::invalid_argument& exception) { error = exception.what(); }
    catch (const Json::exception&) { error = "OPC UA JSON 语法、类型或数值范围错误"; }
    return false;
}
} // namespace industrial_gateway::drivers::opcua

