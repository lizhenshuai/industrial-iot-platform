#pragma once
#include "industrial_gateway/config/ConfigApplyResult.hpp"
#include <string>

namespace industrial_gateway::application::messaging
{
// wire 格式：{"request_id":"..."或null,"status":"Applied/Rejected/Failed","error_message":"..."}。
// 不含配置版本或配置正文；只有整份应用成功才可发送 Applied。
class ConfigAckSerializer final
{
public:
    // 失败保持 json 原值；输入字段非法或含无效 UTF-8 时返回 false。
    static bool serialize(const ::industrial_gateway::config::ConfigApplyResult& result,
                          std::string& json, std::string& error_message);
};
} // namespace industrial_gateway::application::messaging
