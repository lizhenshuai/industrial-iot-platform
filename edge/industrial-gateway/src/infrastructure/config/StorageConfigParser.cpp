
#include "StorageConfigParser.hpp"

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
// 解析 SQLite 存储配置
// ==================================================

bool StorageConfigParser::parse(
    const YAML::Node& root,
    ::industrial_gateway::config::StorageConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();

    // 使用临时对象，避免解析失败时修改原配置
    ::industrial_gateway::config::StorageConfig candidate;


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
    // 2. 获取 storage 节点
    // ==================================================

    const YAML::Node storage = root["storage"];

    // storage 不存在，使用默认配置
    if (!storage.IsDefined())
    {
        config = std::move(candidate);
        return true;
    }

    if (!storage.IsMap())
    {
        error_message =
            "storage 必须是映射类型";

        return false;
    }


    // ==================================================
    // 3. 检查未知配置项
    // ==================================================

    if (
        !validateKnownKeys(
            storage,
            "storage",
            {
                "database_path",
                "busy_timeout_ms"
            },
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 4. 解析数据库文件路径
    // ==================================================

    if (
        !readOptionalScalar(
            storage,
            "database_path",
            "storage.database_path",
            candidate.database_path,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 5. 解析数据库忙等待超时时间
    // ==================================================

    if (
        !readOptionalScalar(
            storage,
            "busy_timeout_ms",
            "storage.busy_timeout_ms",
            candidate.busy_timeout_ms,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 6. 解析成功，提交配置
    // ==================================================

    config = std::move(candidate);

    return true;
}

} // namespace industrial_gateway::infrastructure::config
