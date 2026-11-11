#include "MqttConfigParser.hpp"

#include <yaml-cpp/yaml.h>

#include <initializer_list>
#include <string>
#include <utility>


namespace industrial_gateway::infrastructure::config
{

namespace
{

// ==================================================
// 检查未知配置项
// ==================================================

bool validateKnownKeys(
    const YAML::Node& parent,
    const std::string& parent_name,
    std::initializer_list<const char*> allowed_keys,
    std::string& error_message
)
{
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
// 读取可选标量配置
// ==================================================

template<typename T>
bool readOptionalScalar(
    const YAML::Node& parent,
    const std::string& key,
    const std::string& field_name,
    T& value,
    std::string& error_message
)
{
    const YAML::Node node = parent[key];

    // 字段不存在，保留默认值
    if (!node.IsDefined())
    {
        return true;
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

} // namespace


// ==================================================
// 解析 MQTT 配置
// ==================================================

bool MqttConfigParser::parse(
    const YAML::Node& root,
    ::industrial_gateway::config::MqttConfig& config,
    std::string& error_message
) const
{
    // 清理上一次的错误信息
    error_message.clear();

    // 使用临时对象，避免解析失败时修改原配置
    ::industrial_gateway::config::MqttConfig candidate;


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
    // 2. 获取 mqtt 节点
    // ==================================================

    const YAML::Node mqtt = root["mqtt"];

    // mqtt 不存在，使用默认配置
    if (!mqtt.IsDefined())
    {
        config = std::move(candidate);
        return true;
    }

    if (!mqtt.IsMap())
    {
        error_message =
            "mqtt 必须是映射类型";

        return false;
    }


    // ==================================================
    // 3. 检查未知配置项
    // ==================================================

    if (
        !validateKnownKeys(
            mqtt,
            "mqtt",
            {
                "enabled",
                "host",
                "port",
                "client_id",
                "keep_alive"
            },
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 4. 解析 MQTT 启用状态
    // ==================================================

    if (
        !readOptionalScalar(
            mqtt,
            "enabled",
            "mqtt.enabled",
            candidate.enabled,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 5. 解析 MQTT Broker 地址
    // ==================================================

    if (
        !readOptionalScalar(
            mqtt,
            "host",
            "mqtt.host",
            candidate.host,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 6. 解析 MQTT Broker 端口
    // ==================================================

    if (
        !readOptionalScalar(
            mqtt,
            "port",
            "mqtt.port",
            candidate.port,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 7. 解析 MQTT 客户端 ID
    // ==================================================

    if (
        !readOptionalScalar(
            mqtt,
            "client_id",
            "mqtt.client_id",
            candidate.client_id,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 8. 解析 MQTT 心跳间隔
    // ==================================================

    if (
        !readOptionalScalar(
            mqtt,
            "keep_alive",
            "mqtt.keep_alive",
            candidate.keep_alive,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 9. 解析成功，提交配置
    // ==================================================

    config = std::move(candidate);

    return true;
}

} // namespace industrial_gateway::infrastructure::config