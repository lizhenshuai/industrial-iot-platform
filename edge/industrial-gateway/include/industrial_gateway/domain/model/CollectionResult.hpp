#pragma once

#include "industrial_gateway/domain/enums/CollectionStatus.hpp"
#include "industrial_gateway/domain/model/TelemetryRecord.hpp"

#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::domain::model
{

// ==================================================
// 一个设备在当前频率任务中的完整采集结果
// ==================================================
//
// 对应 CollectionTask::devices 中的一个 DeviceCollectionTask。
// IProtocolConnector::readPoints() 按任务中的设备顺序返回结果集合。
//
// Success：
// 本设备在当前任务中的全部点位读取、解码成功。
// records 包含全部请求点位，顺序与任务中的 properties 一致；
// error_message 为空。
//
// Failure：
// 本设备的完整读取失败，例如未连接、超时、协议异常或解码失败。
// records 必须为空，error_message 描述失败原因。
// 已取得的部分点位值不能作为本次成功结果返回。
//
// 只评价当前频率任务包含的点位，不评价同设备其他频率的点位。
// 一个设备失败不改变同一任务内其他设备的独立结果。
//
// 这是协议驱动与组包层之间的内部结果，不是 MQTT 数据包：
// - TelemetryPacketBuilder 将 device_id、status、records 转换为设备数据。
// - error_message 用于运行日志，不自动加入遥测 JSON。
// - 包 ID 和 UTC 完成时间由上层在整个频率任务完成后生成、记录。
// - 连接及频率上下文由本次 CollectionTask 提供，不在每个结果中重复。
//
// 本结构不执行校验；驱动返回结果时必须满足上述约定。
//
// ==================================================

struct CollectionResult
{
    // 设备 ID，与对应 DeviceCollectionTask::device_id 一致。
    std::int64_t device_id{0};

    // 默认失败，只有完整读取和解码成功后才设置为 Success。
    enums::CollectionStatus status{enums::CollectionStatus::Failure};

    // 成功时保存全部点位记录；失败时为空，不复用上一轮的值。
    std::vector<TelemetryRecord> records;

    // 设备级失败原因。成功时清空，失败时由驱动填写。
    std::string error_message;
};

} // namespace industrial_gateway::domain::model
