#pragma once

#include "industrial_gateway/domain/model/CollectionResult.hpp"
#include "industrial_gateway/domain/model/CollectionTask.hpp"
#include "industrial_gateway/domain/model/TelemetryPacket.hpp"

#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::application::collection
{

// ==================================================
// 将一个频率任务的设备采集结果转换为完整遥测包
// ==================================================
//
// 无运行状态，不创建线程、不执行采集、不操作队列、不序列化 JSON。
// 每次只处理一个 CollectionTask，不跨连接或跨频率合并结果。
//
// 包 ID 和采集完成时间由 ConnectionWorker 提供：
// - ID 在本轮采集完成后生成，调用方保证跨连接、跨重启的唯一性。
// - 时间取整个任务完成时的 UTC Unix 毫秒，不取入队或发布时刻。
// - 入队重试应复用已经构建的包，不重新生成 ID 或获取时间。
//
// ==================================================

class TelemetryPacketBuilder final
{
public:
    // task：本轮频率任务定义。
    // results：按 task.devices 顺序排列的设备级结果，一设备一个结果。
    // message_id：本次包 ID，必须非空且不能包含空字符。
    // completed_at_ms：任务采集完成时的 UTC Unix 毫秒，必须非负。
    // packet：输出包，仅在全部校验和构建成功后更新。
    // error_message：组包失败原因，包含设备或点位位置；成功时清空。
    //
    // Success 设备必须包含当前任务的全部点位，顺序与 properties 一致，
    // 且 error_message 为空。拒绝未知状态、无效值和非有限浮点值。
    // Failure 设备必须 records 为空并提供失败原因，不能携带部分成功数据。
    // 设备失败属于正常采集结果，即使所有设备失败，也可以生成完整包。
    // 内部失败原因不写入遥测包，调用方仍可使用原始 results 记录日志。
    //
    // true：成功构建一个完整包。
    // false：输入不符合约定，packet 保持原样，不输出部分包，
    //        也不自动将错误的 Success 结果改写为 Failure。
    // 输入 task、results 和 message_id 均不被修改或移动。
    // 内存分配等执行异常不转换为输入校验失败，交由上层处理。
    [[nodiscard]]
    bool build(
        const domain::model::CollectionTask& task,
        const std::vector<domain::model::CollectionResult>& results,
        const std::string& message_id,
        std::int64_t completed_at_ms,
        domain::model::TelemetryPacket& packet,
        std::string& error_message
    ) const;
};

} // namespace industrial_gateway::application::collection
