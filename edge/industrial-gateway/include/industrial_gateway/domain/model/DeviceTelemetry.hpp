#pragma once

#include "industrial_gateway/domain/enums/CollectionStatus.hpp"
#include "industrial_gateway/domain/model/TelemetryRecord.hpp"

#include <cstdint>
#include <vector>

namespace industrial_gateway::domain::model
{

// 一个设备的采集结果。Failure 时 records 应为空。
// 部分点位失败的处理由采集端决定，当前协议不提供点位级状态。
struct DeviceTelemetry
{
    std::int64_t device_id{0};
    enums::CollectionStatus status{enums::CollectionStatus::Failure};
    std::vector<TelemetryRecord> records;
};

} // namespace industrial_gateway::domain::model
