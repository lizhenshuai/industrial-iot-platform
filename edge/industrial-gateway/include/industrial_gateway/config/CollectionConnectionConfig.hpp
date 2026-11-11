#pragma once

#include <string>


namespace industrial_gateway::config
{

// ==================================================
// 平台下发的单个采集连接配置
// ==================================================
//
// 对应采集配置快照 connections 数组中的一个元素。
// 与使用 config_file 的静态 ConnectionDefinition 分开，直接携带协议配置。
// 公共层只处理连接身份和协议类型，设备、点位等内容由协议解析器解释。
// 本结构仅保存数据，不执行 JSON 解析、校验或驱动创建。
//
// ==================================================

struct CollectionConnectionConfig final
{
    // 连接实例唯一 ID，必须非空，在同一快照内不能重复。
    // 与 IProtocolConnector::connectionId()、CollectionTask::connection_id 一致。
    std::string connection_id;

    // 协议工厂注册名称，必须非空，例如 modbus_tcp、opcua。
    // 使用字符串，新增协议时无需扩展公共协议枚举。
    std::string protocol;

    // 消息中 config 对象的完整 JSON 文本，必须表示一个 JSON 对象。
    // 字段名 config_json 仅用于 C++；MQTT 消息仍使用 "config": {...}，
    // 不把协议配置作为转义后的 JSON 字符串嵌入消息。
    //
    // 公共解析器负责提取对象；协议解析器负责解释和校验对象内容。
    // 例如 Modbus TCP 对象包含 connection、devices，以及设备下的 points。
    // 这里拥有文本副本，不引用 MQTT 回调中的消息缓冲区，也不绑定 JSON 库。
    //
    // 空字符串不是有效配置。比较配置变化时不能只比较原始文本，
    // 因为空白或对象成员顺序变化不代表协议配置发生变化。
    std::string config_json;
};

} // namespace industrial_gateway::config
