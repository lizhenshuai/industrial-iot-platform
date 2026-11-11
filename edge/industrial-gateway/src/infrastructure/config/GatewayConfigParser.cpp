#include "GatewayConfigParser.hpp"

#include "LoggerConfigParser.hpp"
#include "MqttConfigParser.hpp"
#include "StorageConfigParser.hpp"

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


namespace industrial_gateway::infrastructure::config
{

namespace
{

// ==================================================
// 检查未知及重复配置项
// ==================================================

bool validateKnownKeys(
    const YAML::Node& parent,
    const std::string& parent_name,
    std::initializer_list<const char*> allowed_keys,
    std::string& error_message
)
{
    std::unordered_set<std::string> seen_keys;
    for (const auto& entry : parent)
    {
        const YAML::Node key = entry.first;

        // 配置项名称必须是标量
        if (!key.IsScalar())
        {
            error_message =
                parent_name + " 配置项名称必须是标量";

            return false;
        }

        std::string key_name;

        try
        {
            key_name = key.as<std::string>();
        }
        catch (const YAML::Exception& e)
        {
            error_message =
                parent_name + " 配置项名称解析失败: " +
                e.what();

            return false;
        }

        // 必须在按键读取前拒绝重复键，避免同一字段的取值产生歧义。
        if (!seen_keys.insert(key_name).second)
        {
            error_message = "重复配置项: " + parent_name + "." + key_name;
            return false;
        }

        // 检查是否属于允许的配置项
        bool known = false;

        for (const char* allowed_key : allowed_keys)
        {
            if (key_name == allowed_key)
            {
                known = true;
                break;
            }
        }

        if (!known)
        {
            error_message =
                "未知配置项: " +
                parent_name + "." + key_name;

            return false;
        }
    }

    return true;
}


// ==================================================
// 读取必填标量配置
// ==================================================

template<typename T>
bool readRequiredScalar(
    const YAML::Node& parent,
    const std::string& key,
    const std::string& field_name,
    T& value,
    std::string& error_message
)
{
    const YAML::Node node = parent[key];

    // 必填字段必须存在
    if (!node.IsDefined())
    {
        error_message =
            field_name + " 配置项缺失";

        return false;
    }

    // 必须是标量
    if (!node.IsScalar())
    {
        error_message =
            field_name + " 必须是标量类型";

        return false;
    }

    try
    {
        value = node.as<T>();
        return true;
    }
    catch (const YAML::Exception& e)
    {
        error_message =
            field_name + " 类型或格式错误: " +
            e.what();

        return false;
    }
}

// ==================================================
// 解析可选的静态连接列表
// ==================================================

bool parseConnections(
    const YAML::Node& root,
    std::vector<::industrial_gateway::config::ConnectionDefinition>& connections,
    std::string& error_message
)
{
    std::string path = "connections";
    try
    {
        const YAML::Node nodes = root["connections"];
        if (!nodes.IsDefined())
        {
            connections.clear();
            return true;
        }
        if (!nodes.IsSequence())
        {
            error_message = "connections 必须是序列类型；没有采集连接时使用 []，不能使用 null";
            return false;
        }

        std::vector<::industrial_gateway::config::ConnectionDefinition> candidate;
        candidate.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            path = "connections[" + std::to_string(i) + "]";
            const YAML::Node node = nodes[i];
            if (!node.IsMap())
            {
                error_message = path + " 必须是映射类型";
                return false;
            }
            if (!validateKnownKeys(node, path,
                                   {"connection_id", "protocol", "config_file"},
                                   error_message))
            {
                return false;
            }

            ::industrial_gateway::config::ConnectionDefinition definition;
            if (!readRequiredScalar(node, "connection_id", path + ".connection_id",
                                    definition.connection_id, error_message) ||
                !readRequiredScalar(node, "protocol", path + ".protocol",
                                    definition.protocol, error_message) ||
                !readRequiredScalar(node, "config_file", path + ".config_file",
                                    definition.config_file, error_message))
            {
                return false;
            }

            // 此处只解析结构；不固定协议类型、不读取文件，也不按工作目录解析路径。
            candidate.push_back(std::move(definition));
        }

        connections = std::move(candidate);
        return true;
    }
    catch (const YAML::Exception& e)
    {
        error_message = path + " 解析失败: " + e.what();
        return false;
    }
}

} // namespace


// ==================================================
// 解析网关整体配置
// ==================================================

bool GatewayConfigParser::parse(
    const YAML::Node& root,
    ::industrial_gateway::config::GatewayConfig& config,
    std::string& error_message
) const
{
    // 清理上一次的错误信息
    error_message.clear();

    // 使用临时对象，避免解析失败时修改原配置
    ::industrial_gateway::config::GatewayConfig candidate;


    // ==================================================
    // 1. 检查 YAML 根节点
    // ==================================================

    if (!root.IsMap())
    {
        error_message =
            "YAML 根节点必须是映射类型";

        return false;
    }


    // ==================================================
    // 2. 检查顶层未知配置项
    // ==================================================

    if (
        !validateKnownKeys(
            root,
            "root",
            {
                "schema_version",
                "gateway",
                "logging",
                "mqtt",
                "storage",
                "connections"
            },
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 3. 解析 YAML 配置结构版本
    // ==================================================

    if (
        !readRequiredScalar(
            root,
            "schema_version",
            "schema_version",
            candidate.schema_version,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 4. 获取 gateway 节点
    // ==================================================

    const YAML::Node gateway = root["gateway"];

    if (!gateway.IsDefined())
    {
        error_message =
            "gateway 配置段缺失";

        return false;
    }

    if (!gateway.IsMap())
    {
        error_message =
            "gateway 必须是映射类型";

        return false;
    }


    // ==================================================
    // 5. 检查 gateway 下的未知配置项
    // ==================================================

    if (
        !validateKnownKeys(
            gateway,
            "gateway",
            {
                "id"
            },
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 6. 解析网关唯一标识
    // ==================================================

    if (
        !readRequiredScalar(
            gateway,
            "id",
            "gateway.id",
            candidate.gateway_id,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 7. 解析日志配置
    // ==================================================

    LoggerConfigParser logger_parser;

    if (
        !logger_parser.parse(
            root,
            candidate.logging,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 8. 解析 MQTT 配置
    // ==================================================

    MqttConfigParser mqtt_parser;

    if (
        !mqtt_parser.parse(
            root,
            candidate.mqtt,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 9. 解析 SQLite 存储配置
    // ==================================================

    StorageConfigParser storage_parser;

    if (
        !storage_parser.parse(
            root,
            candidate.storage,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 10. 解析可选静态连接列表
    // ==================================================

    if (!parseConnections(root, candidate.connections, error_message))
    {
        return false;
    }


    // ==================================================
    // 11. 解析成功，提交完整配置
    // ==================================================

    config = std::move(candidate);

    return true;
}

} // namespace industrial_gateway::infrastructure::config
