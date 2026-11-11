#pragma once

#include "industrial_gateway/config/ConnectionDefinition.hpp"
#include "industrial_gateway/config/LoggerConfig.hpp"
#include "industrial_gateway/config/MqttConfig.hpp"
#include "industrial_gateway/config/StorageConfig.hpp"

#include <string>
#include <vector>


// 前向声明，避免引入完整的 GatewayConfig 定义
namespace industrial_gateway::config
{
    struct GatewayConfig;
}


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// 配置校验器
// ==================================================

class ConfigValidator final
{
public:

    // ==================================================
    // 日志配置校验
    // ==================================================

    // 校验日志配置是否合法
    //
    // config:        待校验的日志配置
    // error_message: 校验失败时的错误信息
    //
    // 返回值：
    // true  - 校验通过
    // false - 校验失败
    [[nodiscard]]
    bool validateLoggerConfig(
        const ::industrial_gateway::config::LoggerConfig& config,
        std::string& error_message
    ) const;


    // ==================================================
    // MQTT 配置校验
    // ==================================================

    // 校验 MQTT 配置是否合法
    //
    // config:        待校验的 MQTT 配置
    // error_message: 校验失败时的错误信息
    //
    // 返回值：
    // true  - 校验通过
    // false - 校验失败
    [[nodiscard]]
    bool validateMqttConfig(
        const ::industrial_gateway::config::MqttConfig& config,
        std::string& error_message
    ) const;


    // ==================================================
    // SQLite 存储配置校验
    // ==================================================

    // 校验 SQLite 存储配置是否合法
    //
    // config:        待校验的存储配置
    // error_message: 校验失败时的错误信息
    //
    // 返回值：
    // true  - 校验通过
    // false - 校验失败
    [[nodiscard]]
    bool validateStorageConfig(
        const ::industrial_gateway::config::StorageConfig& config,
        std::string& error_message
    ) const;


    // ==================================================
    // 静态采集连接定义校验
    // ==================================================

    // 校验 connection_id、protocol、config_file 非空、非全空白且不含空字符，
    // 并确保 connection_id 在列表中唯一。空列表合法，表示不启动采集连接。
    // 字符串按原值精确比较，不自动去除空白或转换大小写。
    //
    // 不限定协议名称集合，协议是否注册由 ConnectorFactoryRegistry 检查。
    // 不读取协议文件、不检查文件是否存在，也不执行协议专属参数校验。
    // config_file 可以是相对路径，由上层在交给工厂前按主配置目录解析。
    // 本接口校验静态文件启动定义，不定义 MQTT 配置消息或持久化格式。
    //
    // true：校验成功，清空 error_message。
    // false：校验失败，error_message 包含连接下标、字段及失败原因。
    // 不修改输入配置。
    [[nodiscard]]
    bool validateConnections(
        const std::vector<::industrial_gateway::config::ConnectionDefinition>& connections,
        std::string& error_message
    ) const;


    // ==================================================
    // 网关整体配置校验
    // ==================================================

    // 校验完整网关配置是否合法
    //
    // config:        待校验的网关整体配置
    // error_message: 校验失败时的错误信息
    //
    // 返回值：
    // true  - 校验通过
    // false - 校验失败
    //
    // 内部依次校验：
    // 1. YAML 配置结构版本
    // 2. 网关唯一标识
    // 3. 日志配置
    // 4. MQTT 配置
    // 5. SQLite 存储配置
    // 6. 静态采集连接定义
    [[nodiscard]]
    bool validateGatewayConfig(
        const ::industrial_gateway::config::GatewayConfig& config,
        std::string& error_message
    ) const;

};

} // namespace industrial_gateway::infrastructure::config
