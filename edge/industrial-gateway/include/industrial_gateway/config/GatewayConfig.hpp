
#pragma once

#include "industrial_gateway/config/ConnectionDefinition.hpp"
#include "industrial_gateway/config/LoggerConfig.hpp"
#include "industrial_gateway/config/MqttConfig.hpp"
#include "industrial_gateway/config/StorageConfig.hpp"

#include <string>
#include <vector>


namespace industrial_gateway::config
{

// ==================================================
// 网关整体配置
// ==================================================

struct GatewayConfig
{
    // YAML 配置结构版本
    int schema_version = 1;

    // 网关唯一标识
    std::string gateway_id = "industrial-gateway-001";

    // 日志配置
    LoggerConfig logging;

    // MQTT 配置
    MqttConfig mqtt;

    // SQLite 存储配置
    StorageConfig storage;

    // 静态采集连接定义，按配置顺序创建连接实例。
    // 默认空列表表示不启动协议采集连接。
    // connection_id 在列表中必须唯一，同一种协议可以配置多个实例。
    // 各协议的设备、点位、寻址和解码配置由对应 config_file 提供。
    // 配置加载流程负责校验公共字段，并在交给工厂前以主配置文件目录
    // 为基准，将相对 config_file 路径解析为绝对路径。
    std::vector<ConnectionDefinition> connections;
};

} // namespace industrial_gateway::config
