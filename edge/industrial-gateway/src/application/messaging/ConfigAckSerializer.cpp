#include "ConfigAckSerializer.hpp"
#include <nlohmann/json.hpp>
#include <utility>

namespace industrial_gateway::application::messaging
{
bool ConfigAckSerializer::serialize(const ::industrial_gateway::config::ConfigApplyResult& result,
                                    std::string& json, std::string& error_message)
{
    using Status = ::industrial_gateway::config::ConfigApplyStatus;
    error_message.clear();
    const char* status = nullptr;
    switch (result.status)
    {
        case Status::Applied: status = "Applied"; break;
        case Status::Rejected: status = "Rejected"; break;
        case Status::Failed: status = "Failed"; break;
        default: error_message = "未知配置 ACK 状态"; return false;
    }
    if ((result.request_id && (result.request_id->find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
                              result.request_id->find('\0') != std::string::npos)) ||
        (result.status == Status::Applied && (!result.request_id || !result.error_message.empty())) ||
        (result.status != Status::Applied && result.error_message.empty()))
    {
        error_message = "配置 ACK 字段与处理状态不一致";
        return false;
    }
    try
    {
        nlohmann::json value = {{"status", status}, {"error_message", result.error_message}};
        value["request_id"] = result.request_id ? nlohmann::json(*result.request_id) : nlohmann::json(nullptr);
        auto candidate = value.dump();
        json = std::move(candidate);
        return true;
    }
    catch (const nlohmann::json::exception&)
    {
        error_message = "配置 ACK JSON 序列化失败";
        return false;
    }
}
} // namespace industrial_gateway::application::messaging
