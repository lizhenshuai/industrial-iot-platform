#include "TelemetryPacketBuilder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>


namespace industrial_gateway::application::collection
{
namespace
{

bool validName(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos &&
           value.find('\0') == std::string::npos;
}

bool fail(std::string& error_message, const std::string& path, const std::string& reason)
{
    error_message = "TelemetryPacketBuilder: " + path + ": " + reason;
    return false;
}

} // namespace


bool TelemetryPacketBuilder::build(
    const domain::model::CollectionTask& task,
    const std::vector<domain::model::CollectionResult>& results,
    const std::string& message_id,
    std::int64_t completed_at_ms,
    domain::model::TelemetryPacket& packet,
    std::string& error_message
) const
{
    error_message.clear();
    if (!validName(message_id))
        return fail(error_message, "message_id", "不能为空、全空白或包含空字符");
    if (completed_at_ms < 0)
        return fail(error_message, "completed_at_ms", "UTC Unix 毫秒时间不能为负数");
    if (!validName(task.connection_id))
        return fail(error_message, "task.connection_id", "不能为空、全空白或包含空字符");
    if (task.interval.count() <= 0)
        return fail(error_message, "task.interval", "采集周期必须大于 0");
    if (task.devices.empty())
        return fail(error_message, "task.devices", "设备集合不能为空");
    if (results.size() != task.devices.size())
        return fail(error_message, "results", "结果数量必须与任务设备数量一致");

    // 在临时对象中构建，任何校验失败都不改变调用方已有的 packet。
    domain::model::TelemetryPacket candidate;
    candidate.message_id = message_id;
    candidate.timestamp_ms = completed_at_ms;
    candidate.devices.reserve(task.devices.size());

    std::unordered_set<std::int64_t> device_ids;
    for (std::size_t i = 0; i < task.devices.size(); ++i)
    {
        const auto& device_task = task.devices[i];
        const auto& result = results[i];
        const auto path = "devices[" + std::to_string(i) + "] (device_id=" +
                          std::to_string(device_task.device_id) + ")";
        if (!device_ids.insert(device_task.device_id).second)
            return fail(error_message, path, "任务中的设备 ID 重复");
        if (device_task.properties.empty())
            return fail(error_message, path + ".properties", "任务点位集合不能为空");

        std::unordered_set<std::string> properties;
        for (const auto& property : device_task.properties)
        {
            if (!validName(property))
                return fail(error_message, path + ".properties", "点位名称不能为空、全空白或包含空字符");
            if (!properties.insert(property).second)
                return fail(error_message, path + ".properties", "点位名称重复: " + property);
        }
        if (result.device_id != device_task.device_id)
            return fail(error_message, path, "结果设备 ID 与任务顺序不匹配");

        domain::model::DeviceTelemetry device;
        device.device_id = result.device_id;
        device.status = result.status;

        switch (result.status)
        {
            case domain::enums::CollectionStatus::Success:
            {
                if (!result.error_message.empty())
                    return fail(error_message, path, "Success 结果不能携带失败原因");
                if (result.records.size() != device_task.properties.size())
                    return fail(error_message, path + ".records", "Success 必须包含本任务的全部点位");

                for (std::size_t j = 0; j < result.records.size(); ++j)
                {
                    const auto& record = result.records[j];
                    const auto record_path = path + ".records[" + std::to_string(j) + "]";
                    if (record.property != device_task.properties[j])
                        return fail(error_message, record_path, "点位名称与任务顺序不匹配");
                    if (record.value.valueless_by_exception())
                        return fail(error_message, record_path + ".value", "采集值没有有效的类型和值");
                    if (const auto* number = std::get_if<double>(&record.value);
                        number && !std::isfinite(*number))
                        return fail(error_message, record_path + ".value", "Success 结果不能包含 NaN 或 Infinity");
                }
                // 保留 TelemetryValue 的实际类型，不转换整数精度或改写字符串。
                device.records = result.records;
                break;
            }

            case domain::enums::CollectionStatus::Failure:
            {
                if (!result.records.empty())
                    return fail(error_message, path + ".records", "Failure 结果必须为空，不能携带部分成功数据");
                if (result.error_message.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
                    return fail(error_message, path + ".error_message", "Failure 结果必须提供失败原因");
                // 失败设备仍进入包，records 保持为空；错误文本留给上层日志。
                break;
            }

            default:
                return fail(error_message, path + ".status", "未知采集状态");
        }
        candidate.devices.push_back(std::move(device));
    }

    packet = std::move(candidate);
    return true;
}

} // namespace industrial_gateway::application::collection
