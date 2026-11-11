
#pragma once


namespace industrial_gateway::domain::enums
{

// ==================================================
// 采集状态
// ==================================================
//
// Success : 成功获取并解析采集数据
// Failure : 采集失败，或未能获得有效的协议数据
//
enum class CollectionStatus
{
    Success,
    Failure
};

} // namespace industrial_gateway::domain::enums
