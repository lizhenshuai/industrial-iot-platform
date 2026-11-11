#pragma once

#include "OpcUaConfig.hpp"
#include <string>

namespace YAML { class Node; }

namespace industrial_gateway::drivers::opcua
{
// 两种来源共享字段和语义校验；拒绝未知字段、重复键及显式 null。
// 只校验格式，不连接服务器、不检查远程节点是否存在。失败保留输出原值。
class OpcUaConfigParser final
{
public:
    [[nodiscard]] bool parse(const YAML::Node& root, OpcUaConfig& config, std::string& error) const;
    [[nodiscard]] bool parseJson(const std::string& text, OpcUaConfig& config, std::string& error) const;
    [[nodiscard]] bool validate(const OpcUaConfig& config, std::string& error) const;

    // 主机大小写、IP 表示和默认 4840 端口规范化；路径保留区分。
    // 不做 DNS 查询，不合并域名别名；非法地址抛出 invalid_argument。
    static std::string normalizeEndpoint(const std::string& url);
};
} // namespace industrial_gateway::drivers::opcua
