
#include "LoggerConfigParser.hpp"
#include "industrial_gateway/common/LogLevel.hpp"
#include <yaml-cpp/yaml.h>

#include <initializer_list>
#include <limits>
#include <string>
#include <utility>


namespace industrial_gateway::infrastructure::config
{

namespace
{

// ==================================================
// 检查未知配置项
// ==================================================
// 检查配置项的配置内容是否合理，就是根据key，看到下面的value是否合理
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

        // 配置项名称必须是标量，标量表示单个值
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
// 读取配置信息，读取正确，就放入
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
            field_name + " 类型或格式错误: " + e.what();

        return false;
    }
}


// ==================================================
// 读取非负整数配置
// ==================================================
// 读取整数的配置信息
bool readOptionalSize(
    const YAML::Node& parent,
    const std::string& key,
    const std::string& field_name,
    std::size_t& value,
    std::string& error_message
)
{
    const YAML::Node node = parent[key];

    // 字段不存在，保留默认值
    if (!node.IsDefined())
    {
        return true;
    }

    if (!node.IsScalar())
    {
        error_message =
            field_name + " 必须是整数";

        return false;
    }

    long long parsed_value = 0;

    try
    {
        parsed_value = node.as<long long>();
    }
    catch (const YAML::Exception& e)
    {
        error_message =
            field_name + " 整数格式错误: " + e.what();

        return false;
    }

    // 检查负数
    if (parsed_value < 0)
    {
        error_message =
            field_name + " 不能为负数";

        return false;
    }

    // 检查 size_t 类型范围
    if (
        static_cast<unsigned long long>(parsed_value) >
        static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max()
        )
    )
    {
        error_message =
            field_name + " 超出 size_t 类型范围";

        return false;
    }

    value = static_cast<std::size_t>(parsed_value);

    return true;
}


// ==================================================
// 解析日志等级
// ==================================================
// 读取日志等级信息
bool parseLogLevel(
    const std::string& value,
    common::LogLevel& level,
    std::string& error_message
)
{
    if (value == "trace")
    {
        level = common::LogLevel::Trace;
    }
    else if (value == "debug")
    {
        level = common::LogLevel::Debug;
    }
    else if (value == "info")
    {
        level = common::LogLevel::Info;
    }
    else if (value == "warn")
    {
        level = common::LogLevel::Warn;
    }
    else if (value == "error")
    {
        level = common::LogLevel::Error;
    }
    else if (value == "critical")
    {
        level = common::LogLevel::Critical;
    }
    else
    {
        error_message =
            "不支持的日志等级: " + value;

        return false;
    }

    return true;
}


// ==================================================
// 解析日志滚动策略
// ==================================================
// 读取解析滚动策略
bool parseRotationStrategy(
    const std::string& value,
    ::industrial_gateway::config::LogRotationStrategy& strategy,
    std::string& error_message
)
{
    if (value == "size")
    {
        strategy = ::industrial_gateway::config::LogRotationStrategy::Size;
    }
    else
    {
        error_message =
            "不支持的日志滚动策略: " + value;

        return false;
    }

    return true;
}

} // namespace


// ==================================================
// 解析日志配置
// ==================================================
// 完整的解析日志配置
bool LoggerConfigParser::parse(
    const YAML::Node& root,
    ::industrial_gateway::config::LoggerConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();

    // 使用临时对象，避免解析失败时修改原配置
    ::industrial_gateway::config::LoggerConfig candidate;

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
    // 2. 获取 logging 节点
    // ==================================================

    const YAML::Node logging = root["logging"];

    // logging 不存在，使用默认配置
    if (!logging.IsDefined())
    {
        config = std::move(candidate);
        return true;
    }

    if (!logging.IsMap())
    {
        error_message =
            "logging 必须是映射类型";

        return false;
    }

    // 检查 logging 下是否存在未知配置项
    if (
        !validateKnownKeys(
            logging,
            "logging",
            {
                "level",
                "console_enabled",
                "file_enabled",
                "file_path",
                "rotation"
            },
            error_message
        )
    )
    {
        return false;
    }

    // ==================================================
    // 3. 解析日志等级
    // ==================================================

    // 只有配置了 level 才进行解析
    // 未配置时保留 LoggerConfig 中的默认值 Info
    if (logging["level"].IsDefined())
    {
        std::string level;

        if (
            !readOptionalScalar(
                logging,
                "level",
                "logging.level",
                level,
                error_message
            )
        )
        {
            return false;
        }

        if (
            !parseLogLevel(
                level,
                candidate.level,
                error_message
            )
        )
        {
            return false;
        }
    }

    // ==================================================
    // 4. 解析控制台输出配置
    // ==================================================

    if (
        !readOptionalScalar(
            logging,
            "console_enabled",
            "logging.console_enabled",
            candidate.console_enabled,
            error_message
        )
    )
    {
        return false;
    }

    // ==================================================
    // 5. 解析文件输出配置
    // ==================================================

    if (
        !readOptionalScalar(
            logging,
            "file_enabled",
            "logging.file_enabled",
            candidate.file_enabled,
            error_message
        )
    )
    {
        return false;
    }

    if (
        !readOptionalScalar(
            logging,
            "file_path",
            "logging.file_path",
            candidate.file_path,
            error_message
        )
    )
    {
        return false;
    }

    // ==================================================
    // 6. 获取日志滚动配置
    // ==================================================

    const YAML::Node rotation = logging["rotation"];

    if (rotation.IsDefined())
    {
        if (!rotation.IsMap())
        {
            error_message =
                "logging.rotation 必须是映射类型";

            return false;
        }

        // 检查 rotation 下是否存在未知配置项
        if (
            !validateKnownKeys(
                rotation,
                "logging.rotation",
                {
                    "strategy",
                    "max_size_bytes",
                    "max_files"
                },
                error_message
            )
        )
        {
            return false;
        }

        // ----------------------------------------------
        // 6.1 滚动策略
        // ----------------------------------------------

        // 只有配置了 strategy 才进行解析
        // 未配置时保留结构体中的默认值
        if (rotation["strategy"].IsDefined())
        {
            std::string strategy;

            if (
                !readOptionalScalar(
                    rotation,
                    "strategy",
                    "logging.rotation.strategy",
                    strategy,
                    error_message
                )
            )
            {
                return false;
            }

            if (
                !parseRotationStrategy(
                    strategy,
                    candidate.rotation.strategy,
                    error_message
                )
            )
            {
                return false;
            }
        }

        // ----------------------------------------------
        // 6.2 单个文件最大大小
        // ----------------------------------------------

        if (
            !readOptionalSize(
                rotation,
                "max_size_bytes",
                "logging.rotation.max_size_bytes",
                candidate.rotation.max_size_bytes,
                error_message
            )
        )
        {
            return false;
        }

        // ----------------------------------------------
        // 6.3 历史文件数量
        // ----------------------------------------------

        if (
            !readOptionalSize(
                rotation,
                "max_files",
                "logging.rotation.max_files",
                candidate.rotation.max_files,
                error_message
            )
        )
        {
            return false;
        }
    }

    // ==================================================
    // 7. 解析成功，提交配置
    // ==================================================

    config = std::move(candidate);

    return true;
}

} // namespace industrial_gateway::infrastructure::config
