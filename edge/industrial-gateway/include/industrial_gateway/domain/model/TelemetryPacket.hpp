#pragma once

#include "industrial_gateway/domain/model/DeviceTelemetry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace industrial_gateway::domain::model
{

// 入队前组装完整；一个队列元素对应一条 SQLite 记录和一个 MQTT 包。
struct TelemetryPacket
{
    // 创建包时生成，持久化重试和 MQTT 补传保持不变。
    std::string message_id;
    // 包时间：UTC Unix 毫秒，由生产者确定。
    std::int64_t timestamp_ms{0};
    std::vector<DeviceTelemetry> devices;
};

} // namespace industrial_gateway::domain::model
