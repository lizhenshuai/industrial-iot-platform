#pragma once

#include "industrial_gateway/domain/types/TelemetryValue.hpp"

#include <string>

namespace industrial_gateway::domain::model
{

// 包内的一个成功采集点位。采集失败由设备级 status 表达。
struct TelemetryRecord
{
    std::string property;
    types::TelemetryValue value;
};

} // namespace industrial_gateway::domain::model
