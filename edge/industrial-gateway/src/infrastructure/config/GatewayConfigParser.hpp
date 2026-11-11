#pragma once

#include "industrial_gateway/config/GatewayConfig.hpp"

#include <string>


// YAML::Node 前向声明，避免在头文件中引入 yaml-cpp
namespace YAML
{
    class Node;
}


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// 网关整体配置解析器
// ==================================================

class GatewayConfigParser final
{
public:

    // ==================================================
    // 解析网关整体配置
    // ==================================================

    // 解析完整网关配置
    //
    // root:          YAML 配置文件根节点
    // config:        输出解析后的网关整体配置
    // error_message: 解析失败时的错误信息
    //
    // 负责解析：
    // 1. YAML 配置结构版本
    // 2. 网关唯一标识
    // 3. 日志配置
    // 4. MQTT 配置
    // 5. SQLite 存储配置
    // 6. 可选的静态采集连接定义 connections
    //
    // connections 示例：
    // connections:
    //   - connection_id: "modbus_tcp_001"
    //     protocol: "modbus_tcp"
    //     config_file: "connections/modbus_tcp_001.yaml"
    //
    // connections 缺省或为 [] 时，输出空列表；显式 null 不视为缺省。
    // 每个元素必须是映射，三个字段均为必填标量，不允许未知或重复键。
    // 保留输入顺序和字符串原值，不解析协议文件、不转换路径。
    // 非空字段、重复连接 ID 等语义约束由 ConfigValidator 校验。
    // 相对 config_file 路径由上层按主配置目录解析为绝对路径。
    // 此入口仅描述静态文件启动格式，不定义 MQTT 下发格式或持久化位置。
    //
    // 返回值：
    // true  - 解析成功，更新 config；缺省 connections 不保留上次的连接列表
    // false - 解析失败，config 保持原样，error_message 描述字段路径及原因
    [[nodiscard]]
    bool parse(
        const YAML::Node& root,
        ::industrial_gateway::config::GatewayConfig& config,
        std::string& error_message
    ) const;

};

} // namespace industrial_gateway::infrastructure::config
