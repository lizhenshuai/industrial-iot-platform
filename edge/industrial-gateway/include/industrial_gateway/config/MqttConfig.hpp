#pragma once

#include <string>


namespace industrial_gateway::config
{

// ==================================================
// MQTT 配置
// ==================================================

struct MqttConfig
{
    // 是否启用 MQTT
    bool enabled = true;

    // MQTT Broker 地址
    std::string host = "127.0.0.1";

    // MQTT Broker 端口
    int port = 1883;

    // MQTT 客户端 ID
    std::string client_id = "industrial-gateway-001";

    // MQTT 心跳间隔（秒）
    int keep_alive = 60;
};

} // namespace industrial_gateway::config