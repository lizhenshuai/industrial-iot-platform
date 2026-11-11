#include "CollectionConfigParser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


namespace industrial_gateway::infrastructure::config
{
namespace
{

using Json = nlohmann::json;

// 仅用于内部传递配置错误；公共接口转换为 false 和错误信息。
class ConfigError final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void require(bool valid, const std::string& path, const std::string& reason)
{
    if (!valid) throw ConfigError(path + ": " + reason);
}

// 在 JSON 库覆盖同名成员前拒绝重复键，包括协议配置内部的重复键。
// 始终保留解析事件，不使用返回 false 的过滤语义忽略非法配置。
Json parseJson(const std::string& payload)
{
    require(payload.find('\0') == std::string::npos,
            "root", "JSON 消息不能包含未转义的空字符");

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

    // 传入完整范围，严格解析单个 JSON 值；不启用注释扩展。
    return Json::parse(payload.begin(), payload.end(), callback, true, false);
}

void checkObject(const Json& node, const std::string& path,
                 std::initializer_list<const char*> allowed)
{
    require(node.is_object(), path, "必须是 JSON 对象");
    for (auto entry = node.begin(); entry != node.end(); ++entry)
    {
        require(std::any_of(allowed.begin(), allowed.end(),
                    [&](const char* key) { return entry.key() == key; }),
                path + "." + entry.key(), "未知配置项");
    }
}

const Json& requiredField(const Json& parent, const char* key,
                          const std::string& path)
{
    const auto found = parent.find(key);
    require(found != parent.end(), path + "." + key, "缺少必填配置项");
    return *found;
}

std::string readText(const Json& parent, const char* key, const std::string& path)
{
    const auto field = path + "." + key;
    const auto& node = requiredField(parent, key, path);
    require(node.is_string(), field, "必须是字符串");
    const auto& value = node.get_ref<const std::string&>();
    require(value.find_first_not_of(" \t\r\n\f\v") != std::string::npos,
            field, "不能是空字符串或全空白字符串");
    require(value.find('\0') == std::string::npos, field, "不能包含空字符");
    return value;
}

std::int64_t readPositiveInteger(const Json& parent, const char* key,
                                 const std::string& path)
{
    const auto field = path + "." + key;
    const auto& node = requiredField(parent, key, path);
    require(node.is_number_integer(), field, "必须是 JSON 整数");

    // JSON 库可能将正整数保存为 uint64_t，转换前必须检查有符号上限。
    if (node.is_number_unsigned())
    {
        const auto value = node.get<std::uint64_t>();
        require(value > 0 && value <= static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()),
                field, "必须在 1 到 9223372036854775807 之间");
        return static_cast<std::int64_t>(value);
    }

    const auto value = node.get<std::int64_t>();
    require(value > 0, field, "必须是正整数");
    return value;
}

} // namespace


// ==================================================
// 解析完整快照：所有公共字段合法后才提交输出
// ==================================================

bool CollectionConfigParser::parse(
    const std::string& payload,
    ::industrial_gateway::config::CollectionConfigSnapshot& config,
    std::string& error_message,
    std::optional<std::string>* request_id
) const
{
    error_message.clear();
    if (request_id) request_id->reset();
    try
    {
        const auto root = parseJson(payload);
        if (request_id && root.is_object())
        {
            const auto id = root.find("request_id");
            if (id != root.end() && id->is_string())
            {
                const auto& value = id->get_ref<const std::string&>();
                if (value.find_first_not_of(" \t\r\n\f\v") != std::string::npos &&
                    value.find('\0') == std::string::npos)
                    *request_id = value;
            }
        }
        checkObject(root, "root",
                    {"schema_version", "request_id", "connections"});

        ::industrial_gateway::config::CollectionConfigSnapshot candidate;
        const auto schema_version = readPositiveInteger(root, "schema_version", "root");
        require(schema_version == 1, "root.schema_version", "只支持消息结构版本 1");
        candidate.schema_version = static_cast<int>(schema_version);
        candidate.request_id = readText(root, "request_id", "root");

        const auto& connections = requiredField(root, "connections", "root");
        require(connections.is_array(), "root.connections",
                "必须是数组；清空采集连接时使用 []，不能使用 null");

        candidate.connections.reserve(connections.size());
        std::unordered_set<std::string> connection_ids;
        for (std::size_t i = 0; i < connections.size(); ++i)
        {
            const auto path = "root.connections[" + std::to_string(i) + "]";
            const auto& node = connections[i];
            checkObject(node, path, {"connection_id", "protocol", "config"});

            ::industrial_gateway::config::CollectionConnectionConfig connection;
            connection.connection_id = readText(node, "connection_id", path);
            connection.protocol = readText(node, "protocol", path);
            require(connection_ids.insert(connection.connection_id).second,
                    path + ".connection_id", "连接 ID 重复");

            const auto& protocol_config = requiredField(node, "config", path);
            require(protocol_config.is_object(), path + ".config", "必须是 JSON 对象");
            // 不读取本地文件，也不在公共层校验设备、点位和协议名称。
            connection.config_json = protocol_config.dump();
            candidate.connections.push_back(std::move(connection));
        }

        config = std::move(candidate);
        return true;
    }
    catch (const ConfigError& error)
    {
        error_message = error.what();
    }
    catch (const Json::parse_error& error)
    {
        // JSON 库的原始异常可能包含配置值；只返回语法错误位置。
        error_message = "root: JSON 语法错误，字节位置 " + std::to_string(error.byte);
    }
    catch (const Json::out_of_range&)
    {
        error_message = "root: JSON 数值或容器大小超出解析器支持的范围";
    }
    catch (const Json::exception&)
    {
        error_message = "root: JSON 配置解析或协议配置序列化失败";
    }
    return false;
}

} // namespace industrial_gateway::infrastructure::config
