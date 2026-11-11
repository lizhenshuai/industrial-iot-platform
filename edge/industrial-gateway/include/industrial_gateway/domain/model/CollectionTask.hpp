#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::domain::model
{

// ==================================================
// 一个设备在当前频率任务中的采集范围
// ==================================================
//
// 逻辑点位通过所属连接、device_id 和 property 定位。
// 协议驱动根据自身配置映射到寄存器、NodeId 等实际地址。
//
// 第一版将本设备的这些点位作为一个完整读取单元：
// 全部读取、解码成功才返回完整记录；否则返回设备级 Failure。
// Modbus 配置校验必须确保这些点位能够由一次合法请求覆盖。
//
// ==================================================

struct DeviceCollectionTask
{
    // 设备 ID，与最终 TelemetryPacket 中的 device_id 一致。
    // 不是 Modbus Unit ID，协议设备地址由驱动配置维护。
    std::int64_t device_id{0};

    // 本频率下需要采集的逻辑点位名称。
    // 名称对应 TelemetryRecord::property；集合非空，名称非空且不重复。
    // 只包含本任务的点位，不包含同设备其他频率的点位。
    std::vector<std::string> properties;
};


// ==================================================
// 一个连接内、一个采集频率对应的任务定义
// ==================================================
//
// 同连接中相同周期的设备和点位归入同一个任务。
// connection_id + interval 唯一标识该连接配置中的一个频率任务。
//
// 例如 1 秒和 5 秒任务在第 5 秒同时到期时：
// Worker 分别执行两个任务，分别生成两个 TelemetryPacket。
// 一个任务可以包含多个设备，因此整个任务可能执行多个协议请求。
//
// 此结构只描述采集计划，不保存运行状态：
// - 下次到期时间由 PollScheduler 使用单调时钟管理。
// - 任务执行结果由 CollectionResult 表达。
// - 包 ID 和 UTC 完成时间在本轮采集完成后由上层生成、记录。
// - 不携带协议地址、连接句柄或 JSON 数据。
//
// 配置解析及任务构建阶段负责校验以下约束；本结构不执行校验。
// 运行期间任务定义保持不变，不允许控制线程并发修改。
//
// ==================================================

struct CollectionTask
{
    // 所属连接实例 ID，必须非空，与 IProtocolConnector::connectionId() 一致。
    std::string connection_id;

    // 采集周期，必须大于 0；默认 1 秒。
    // 表示目标周期，不保证设备超时或队列阻塞时仍能按时完成。
    std::chrono::milliseconds interval{1000};

    // 本任务包含的设备采集范围，集合非空，同一设备不能重复出现。
    // 驱动按此顺序返回每个设备的采集结果。
    std::vector<DeviceCollectionTask> devices;
};

} // namespace industrial_gateway::domain::model
