#pragma once

#include <optional>
#include <string>


namespace industrial_gateway::config
{

// ==================================================
// 配置请求处理结果
// ==================================================
//
// 这是应用层配置 ACK，与 MQTT 的 PUBACK、SUBACK 无关。
// 枚举的 JSON 字符串映射由后续 ACK 序列化器负责，不发送枚举整数值。
//
// ==================================================

enum class ConfigApplyStatus
{
    // 整份目标配置已完成约定的应用步骤。
    // 不能在收到消息、入队或仅保存配置时返回此状态。
    // 不表示所有设备已连接，也不表示设备已成功完成首次采集。
    Applied,

    // 应用前拒绝请求，例如字段非法或协议不支持。
    // 本次请求没有改变正在运行的采集连接。
    Rejected,

    // 请求处理未能完成，例如持久化或连接创建、替换失败。
    // 运行状态可能尚未改变，也可能只有部分连接发生变化；
    // 此状态不承诺已回滚，也不代表整份目标配置生效。
    Failed
};


// ==================================================
// 配置 ACK 数据模型
// ==================================================
//
// 用于构建 v1/gateways/{gateway_id}/config/ack 的消息体。
// 只保存处理结果，不执行序列化、MQTT 发布、持久化或失败恢复。
// 字段有效性及状态之间的约束由创建结果的配置处理流程保证。
//
// ==================================================

struct ConfigApplyResult final
{
    // 从请求中正确读取并校验后的请求 ID，回复时保留原值。
    // 请求无法解析、字段缺失或字段非法时使用 nullopt，不伪造请求 ID。
    std::optional<std::string> request_id;

    // 默认失败，只有满足对应状态的条件后才显式设置结果。
    ConfigApplyStatus status{ConfigApplyStatus::Failed};

    // Rejected / Failed 时填写原因；Applied 时为空。
    // 用于说明问题，不作为平台判断结果状态的唯一依据。
    // 涉及协议配置时避免包含密码等敏感配置内容。
    std::string error_message;
};

} // namespace industrial_gateway::config
