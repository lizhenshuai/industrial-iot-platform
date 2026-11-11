#pragma once

#include <string>


namespace industrial_gateway::config
{

// ==================================================
// 连接实例定义
// ==================================================
//
// 描述一个连接实例的身份、协议类型和协议配置来源。
// 同一种协议可以配置多个连接实例，各自拥有独立的配置和运行资源。
//
// 第一版用于静态配置启动：
// 1. 主配置解析器读取连接定义并校验公共字段。
// 2. 工厂注册表根据 protocol 选择具体协议的创建函数。
// 3. 协议配置解析器读取 config_file，校验协议配置并构建采集任务。
// 4. 上层创建驱动和 ConnectionWorker，管理运行生命周期。
//
// 不包含主机地址、Unit ID、寄存器地址、NodeId 等协议专属字段，
// 也不包含运行状态、线程或协议连接句柄。
// 本结构仅保存数据，校验及路径解析由配置加载流程完成。
//
// ==================================================

struct ConnectionDefinition final
{
    // 连接实例唯一 ID，必须非空，在同一网关的连接定义中不能重复。
    // 与 IProtocolConnector::connectionId()、CollectionTask::connection_id 一致。
    // 实例运行期间保持不变。
    std::string connection_id;

    // 协议类型，必须非空并与工厂注册键一致。
    // 例如 modbus_tcp、opcua、modbus_rtu。
    // 不使用协议枚举，新增协议时无需修改本结构。
    std::string protocol;

    // 协议专属配置文件路径，必须非空。
    // 主配置中的相对路径以主配置文件所在目录为基准，
    // 不以进程当前工作目录为基准；绝对路径直接使用。
    // 配置加载流程应在交给工厂前将相对路径解析为绝对路径。
    // 文件内容由对应协议的配置解析器读取和校验。
    std::string config_file;
};

} // namespace industrial_gateway::config
