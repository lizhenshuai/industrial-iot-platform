#pragma once

#include "ModbusRtuConfig.hpp"
#include <string>

namespace YAML { class Node; }

namespace industrial_gateway::drivers::modbus_rtu
{
// JSON / YAML 两个入口共用串口、点位字段解析与语义校验。
// 必填 connection.device、devices、device_id、points、property、address；
// 其他字段缺失时使用模型默认值，显式 null、未知字段、重复键均拒绝。
// JSON 整数字段不接受字符串、布尔值或浮点数。失败保留输出原值。
// 不打开串口、不探测从站；文件内容由注册工厂读取。
class ModbusRtuConfigParser final
{
public:
    [[nodiscard]] bool parse(const YAML::Node& root, ModbusRtuConfig& config,
                             std::string& error_message) const;
    [[nodiscard]] bool parseJson(const std::string& config_json, ModbusRtuConfig& config,
                                 std::string& error_message) const;
    // 校验串口参数、从站 1..247、设备/从站唯一性和同设备同周期连续读取范围。
    [[nodiscard]] bool validate(const ModbusRtuConfig& config, std::string& error_message) const;
};
} // namespace industrial_gateway::drivers::modbus_rtu
