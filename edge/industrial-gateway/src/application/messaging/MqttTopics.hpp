#pragma once

#include <string>
#include <string_view>

namespace industrial_gateway::application::messaging
{

// 网关与平台之间的 MQTT Topic 约定，仅负责构造路径。
// 调用方应保证 gateway_id 非空，且不包含 /、+、# 或空字符。
struct MqttTopics final
{
    // 上行：遥测数据上报。
    static std::string telemetry(std::string_view gateway_id)
    {
        return base(gateway_id) + "/telemetry";
    }

    // 上行：网关定期心跳。
    static std::string heartbeat(std::string_view gateway_id)
    {
        return base(gateway_id) + "/heartbeat";
    }

    // 上行：网关在线/离线状态。
    static std::string availability(std::string_view gateway_id)
    {
        return base(gateway_id) + "/availability";
    }

    // 下行：平台下发网关配置。
    static std::string config(std::string_view gateway_id)
    {
        return base(gateway_id) + "/config";
    }

    // 上行：配置执行结果反馈。
    static std::string configAck(std::string_view gateway_id)
    {
        return base(gateway_id) + "/config/ack";
    }

    // 下行：平台下发控制命令。
    static std::string commands(std::string_view gateway_id)
    {
        return base(gateway_id) + "/commands";
    }

    // 上行：命令执行结果反馈。
    static std::string commandsAck(std::string_view gateway_id)
    {
        return base(gateway_id) + "/commands/ack";
    }

private:
    static std::string base(std::string_view gateway_id)
    {
        return "v1/gateways/" + std::string(gateway_id);
    }
};

} // namespace industrial_gateway::application::messaging
