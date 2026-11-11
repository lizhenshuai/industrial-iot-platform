#include "RegisterBuiltInConnectors.hpp"

#include "../drivers/modbus_tcp/RegisterModbusTcpConnector.hpp"
#include "../drivers/modbus_rtu/RegisterModbusRtuConnector.hpp"
#include "../drivers/opcua/RegisterOpcUaConnector.hpp"


namespace industrial_gateway::bootstrap
{

bool registerBuiltInConnectors(
    application::connector::ConnectorFactoryRegistry& registry,
    std::string& error_message
)
{
    error_message.clear();

    std::string registration_error;
    if (!drivers::modbus_tcp::registerModbusTcpConnector(registry, registration_error))
    {
        error_message = "注册内置协议 modbus_tcp 失败: " + registration_error;
        return false;
    }

    if (!drivers::modbus_rtu::registerModbusRtuConnector(registry, registration_error))
    {
        error_message = "注册内置协议 modbus_rtu 失败: " + registration_error;
        return false;
    }

    if (!drivers::opcua::registerOpcUaConnector(registry, registration_error))
    {
        error_message = "注册内置协议 opcua 失败: " + registration_error;
        return false;
    }

    // 后续协议在此显式注册，并逐项检查返回结果。
    // 只有全部协议注册成功，上层才继续创建连接实例。
    return true;
}

} // namespace industrial_gateway::bootstrap
