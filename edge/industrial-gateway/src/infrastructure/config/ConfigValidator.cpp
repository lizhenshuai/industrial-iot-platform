
#include "ConfigValidator.hpp"

#include "industrial_gateway/common/LogLevel.hpp"
#include "industrial_gateway/config/GatewayConfig.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>


namespace industrial_gateway::infrastructure::config
{

namespace
{

// ==================================================
// 配置硬性限制
// ==================================================

// 日志文件总数量上限（包含当前日志文件）
constexpr std::size_t kMaxTotalLogFiles = 50;

// 历史轮转文件数量上限（不包含当前日志文件）
constexpr std::size_t kMaxRotatedLogFiles =
    kMaxTotalLogFiles - 1;

// 单个日志文件最大容量：2 GiB
constexpr std::size_t kMaxLogFileSizeBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

// SQLite 数据库忙等待超时时间上限：60 秒
constexpr int kMaxBusyTimeoutMs = 60000;

// MQTT 端口范围
constexpr int kMinMqttPort = 1;
constexpr int kMaxMqttPort = 65535;

// MQTT Keep Alive 范围（秒）
constexpr int kMinMqttKeepAlive = 0;
constexpr int kMaxMqttKeepAlive = 65535;

// MQTT Client ID 最大长度（字节）
constexpr std::size_t kMaxMqttClientIdLength = 65535;

// 当前支持的 YAML 配置结构版本
constexpr int kSupportedSchemaVersion = 1;


// ==================================================
// 检查字符串是否为空白字符串
// ==================================================

bool isBlank(const std::string& value) noexcept
{
    for (const unsigned char ch : value)
    {
        if (!std::isspace(ch))
        {
            return false;
        }
    }

    return true;
}


// ==================================================
// 检查日志等级是否合法
// ==================================================

bool isValidLogLevel(common::LogLevel level) noexcept
{
    switch (level)
    {
        case common::LogLevel::Trace:
        case common::LogLevel::Debug:
        case common::LogLevel::Info:
        case common::LogLevel::Warn:
        case common::LogLevel::Error:
        case common::LogLevel::Critical:
            return true;

        default:
            return false;
    }
}


// ==================================================
// 检查日志滚动策略是否合法
// ==================================================

bool isValidRotationStrategy(
    ::industrial_gateway::config::LogRotationStrategy strategy
) noexcept
{
    switch (strategy)
    {
        case ::industrial_gateway::config::LogRotationStrategy::Size:
            return true;

        default:
            return false;
    }
}

} // namespace


// ==================================================
// 校验日志配置
// ==================================================

bool ConfigValidator::validateLoggerConfig(
    const ::industrial_gateway::config::LoggerConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();


    // ==================================================
    // 1. 校验日志等级
    // ==================================================

    if (!isValidLogLevel(config.level))
    {
        error_message =
            "logging.level 不合法";

        return false;
    }


    // ==================================================
    // 2. 校验日志输出方式
    // ==================================================

    if (!config.console_enabled && !config.file_enabled)
    {
        error_message =
            "日志输出至少需要启用一种方式";

        return false;
    }


    // ==================================================
    // 3. 校验文件日志配置
    // ==================================================

    // 文件日志未启用时，不需要校验文件相关参数
    if (config.file_enabled)
    {
        // --------------------------------------------------
        // 3.1 校验日志文件路径
        // --------------------------------------------------

        if (isBlank(config.file_path))
        {
            error_message =
                "logging.file_path 不能为空";

            return false;
        }


        // --------------------------------------------------
        // 3.2 校验日志滚动策略
        // --------------------------------------------------

        if (!isValidRotationStrategy(config.rotation.strategy))
        {
            error_message =
                "logging.rotation.strategy 不合法";

            return false;
        }


        // --------------------------------------------------
        // 3.3 校验单个日志文件大小
        // --------------------------------------------------

        if (config.rotation.max_size_bytes == 0)
        {
            error_message =
                "logging.rotation.max_size_bytes 必须大于 0";

            return false;
        }

        if (config.rotation.max_size_bytes > kMaxLogFileSizeBytes)
        {
            error_message =
                "logging.rotation.max_size_bytes 不能超过 2 GiB";

            return false;
        }


        // --------------------------------------------------
        // 3.4 校验历史日志文件数量
        // --------------------------------------------------

        if (config.rotation.max_files == 0)
        {
            error_message =
                "logging.rotation.max_files 必须大于 0";

            return false;
        }

        if (config.rotation.max_files > kMaxRotatedLogFiles)
        {
            error_message =
                "logging.rotation.max_files 不能超过 49"
                "（当前日志文件与历史文件总数最多 50 个）";

            return false;
        }
    }


    return true;
}


// ==================================================
// 校验 MQTT 配置
// ==================================================

bool ConfigValidator::validateMqttConfig(
    const ::industrial_gateway::config::MqttConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();


    // ==================================================
    // 1. 检查 MQTT 是否启用
    // ==================================================

    // MQTT 未启用时，不需要校验连接参数
    if (!config.enabled)
    {
        return true;
    }


    // ==================================================
    // 2. 校验 Broker 地址
    // ==================================================

    if (isBlank(config.host))
    {
        error_message =
            "mqtt.host 不能为空";

        return false;
    }


    // ==================================================
    // 3. 校验 MQTT 端口
    // ==================================================

    if (
        config.port < kMinMqttPort ||
        config.port > kMaxMqttPort
    )
    {
        error_message =
            "mqtt.port 必须在 1 到 65535 之间";

        return false;
    }


    // ==================================================
    // 4. 校验 MQTT 客户端 ID
    // ==================================================

    if (isBlank(config.client_id))
    {
        error_message =
            "mqtt.client_id 不能为空";

        return false;
    }

    if (config.client_id.size() > kMaxMqttClientIdLength)
    {
        error_message =
            "mqtt.client_id 不能超过 65535 字节";

        return false;
    }


    // ==================================================
    // 5. 校验 MQTT 心跳间隔
    // ==================================================

    if (
        config.keep_alive < kMinMqttKeepAlive ||
        config.keep_alive > kMaxMqttKeepAlive
    )
    {
        error_message =
            "mqtt.keep_alive 必须在 0 到 65535 秒之间";

        return false;
    }


    return true;
}


// ==================================================
// 校验 SQLite 存储配置
// ==================================================

bool ConfigValidator::validateStorageConfig(
    const ::industrial_gateway::config::StorageConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();


    // ==================================================
    // 1. 校验数据库文件路径
    // ==================================================

    if (isBlank(config.database_path))
    {
        error_message =
            "storage.database_path 不能为空";

        return false;
    }


    // ==================================================
    // 2. 校验数据库忙等待超时时间
    // ==================================================

    if (config.busy_timeout_ms < 0)
    {
        error_message =
            "storage.busy_timeout_ms 不能为负数";

        return false;
    }

    if (config.busy_timeout_ms > kMaxBusyTimeoutMs)
    {
        error_message =
            "storage.busy_timeout_ms 不能超过 60000 毫秒";

        return false;
    }


    return true;
}


// ==================================================
// 校验静态采集连接定义
// ==================================================

bool ConfigValidator::validateConnections(
    const std::vector<::industrial_gateway::config::ConnectionDefinition>& connections,
    std::string& error_message
) const
{
    error_message.clear();

    // 记录首次出现的位置，使重复 ID 的错误能够定位到两条配置。
    std::unordered_map<std::string, std::size_t> connection_ids;
    for (std::size_t i = 0; i < connections.size(); ++i)
    {
        const auto& connection = connections[i];
        const auto path = "connections[" + std::to_string(i) + "]";
        const auto validate_field = [&](const std::string& value, const char* field)
        {
            if (isBlank(value))
            {
                error_message = path + "." + field + " 不能为空或全空白字符串";
                return false;
            }
            if (value.find('\0') != std::string::npos)
            {
                error_message = path + "." + field + " 不能包含空字符";
                return false;
            }
            return true;
        };

        if (!validate_field(connection.connection_id, "connection_id") ||
            !validate_field(connection.protocol, "protocol") ||
            !validate_field(connection.config_file, "config_file"))
        {
            return false;
        }

        const auto inserted = connection_ids.emplace(connection.connection_id, i);
        if (!inserted.second)
        {
            error_message = path + ".connection_id 与 connections[" +
                std::to_string(inserted.first->second) + "].connection_id 重复: " +
                connection.connection_id;
            return false;
        }
    }

    return true;
}


// ==================================================
// 校验网关整体配置
// ==================================================

bool ConfigValidator::validateGatewayConfig(
    const ::industrial_gateway::config::GatewayConfig& config,
    std::string& error_message
) const
{
    // 清理错误信息
    error_message.clear();


    // ==================================================
    // 1. 校验 YAML 配置结构版本
    // ==================================================

    if (config.schema_version != kSupportedSchemaVersion)
    {
        error_message =
            "schema_version 不支持，当前仅支持版本 1";

        return false;
    }


    // ==================================================
    // 2. 校验网关唯一标识
    // ==================================================

    if (isBlank(config.gateway_id))
    {
        error_message =
            "gateway.id 不能为空";

        return false;
    }


    // ==================================================
    // 3. 校验日志配置
    // ==================================================

    if (!validateLoggerConfig(config.logging, error_message))
    {
        return false;
    }


    // ==================================================
    // 4. 校验 MQTT 配置
    // ==================================================

    if (!validateMqttConfig(config.mqtt, error_message))
    {
        return false;
    }


    // ==================================================
    // 5. 校验 SQLite 存储配置
    // ==================================================

    if (!validateStorageConfig(config.storage, error_message))
    {
        return false;
    }


    // ==================================================
    // 6. 校验静态采集连接定义
    // ==================================================

    if (!validateConnections(config.connections, error_message))
    {
        return false;
    }


    return true;
}

} // namespace industrial_gateway::infrastructure::config
