#pragma once

#include <string>


namespace industrial_gateway::application::connector
{
    class ConnectorFactoryRegistry;
}


namespace industrial_gateway::bootstrap
{

// ==================================================
// 内置协议统一注册入口
// ==================================================
//
// 由程序启动装配代码调用，在 ConnectorManager 创建连接实例前完成注册。
// 当前注册 modbus_tcp、modbus_rtu 和 opcua；新增协议时在对应 .cpp 中增加注册调用，
// 不需要让 Manager、Worker 或调度器依赖具体协议。
//
// 这里只登记工厂，不读取协议配置、不连接设备、不启动采集线程。
// 内置协议编译进程序，不涉及动态库加载。
//
// true：全部注册成功，清空 error_message。
// false：某个协议注册失败，error_message 包含协议名称及具体原因。
// 重复调用会因重复协议注册而失败，不覆盖原工厂。
//
// 多协议注册不是事务：失败时不撤销此前已经完成的注册。
// 调用方应将失败视为启动装配失败，不继续创建连接实例；
// 需要重试整套注册时，应使用新的注册表。
//
// ==================================================

[[nodiscard]]
bool registerBuiltInConnectors(
    application::connector::ConnectorFactoryRegistry& registry,
    std::string& error_message
);

} // namespace industrial_gateway::bootstrap
