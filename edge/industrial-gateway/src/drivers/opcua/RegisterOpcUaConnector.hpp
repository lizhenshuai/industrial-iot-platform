#pragma once

#include <string>


namespace industrial_gateway::application::connector
{
    class ConnectorFactoryRegistry;
}


namespace industrial_gateway::drivers::opcua
{

// 一次登记 opcua 的文件和内存配置工厂，不立即读取配置或创建实例。
// 后续 registry.create() 根据参数类型选择对应入口：
// 1. ConnectionDefinition：读取 config_file 指定的协议 YAML 文件；
//    CollectionConnectionConfig：直接读取 config_json 中的协议 JSON 对象。
// 2. 使用 OpcUaConfigParser 解析并校验配置。
// 3. 共用同一份逻辑，按采集周期构建完整 CollectionTask 列表。
// 4. 创建尚未连接的 OpcUaConnector，返回驱动和任务。
//
// 不创建采集线程、不访问 DataQueue、不调用驱动 connect()。
// 工厂不捕获外部对象，每次创建使用独立配置和驱动实例。
// true 表示注册成功，清空 error_message；重复注册等失败由注册表返回原因。
[[nodiscard]]
bool registerOpcUaConnector(
    application::connector::ConnectorFactoryRegistry& registry,
    std::string& error_message
);

} // namespace industrial_gateway::drivers::opcua
