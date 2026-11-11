#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace industrial_gateway::drivers::opcua
{
struct OpcUaPointConfig
{
    std::string property;
    // 第一版使用稳定的命名空间索引，只支持数字/字符串 NodeId。
    // 例如 ns=2;i=1001、ns=2;s=Device1.Temperature；不做 Browse 路径搜索。
    std::string node_id;
    std::chrono::milliseconds interval{1000};
    // 仅数值可使用非默认缩放；默认保持整数精度，缩放后输出 double。
    double scale{1.0};
    double offset{0.0};
};

struct OpcUaDeviceConfig
{
    // 平台逻辑分组，不是 OPC UA 协议的设备地址。
    std::int64_t device_id{0};
    std::vector<OpcUaPointConfig> points;
};

struct OpcUaConfig
{
    std::string endpoint_url;
    // 单次客户端操作超时，1..60000ms；不是整个多设备任务的总超时。
    std::chrono::milliseconds timeout{3000};
    // 显式限制第一版能力，不接受后静默忽略安全配置。
    std::string security_mode{"None"};
    std::string authentication{"anonymous"};
    std::vector<OpcUaDeviceConfig> devices;
};
} // namespace industrial_gateway::drivers::opcua
