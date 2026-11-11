#pragma once

#include "industrial_gateway/config/CollectionConnectionConfig.hpp"

#include <string>
#include <vector>


namespace industrial_gateway::config
{

// ==================================================
// 一个网关的完整采集配置快照
// ==================================================
//
// 对应 v1/gateways/{gateway_id}/config 的 JSON 消息体。
// 目标网关由 Topic 标识，消息体不重复保存 gateway_id。
// 只包含采集连接配置，不包含 MQTT、日志、数据库等网关基础配置。
//
// 快照描述全部目标连接：旧配置中存在、新快照中缺失的连接应被删除。
// 这是目标状态的数据模型，不代表配置已经持久化或已经应用。
// 解析器与配置管理器负责执行下述约束，本结构自身不执行校验。
//
// ==================================================

struct CollectionConfigSnapshot final
{
    // MQTT 配置消息结构版本，第一版支持 1。
    // 与本地 GatewayConfig 的 YAML 结构版本分别解释，
    // 不表示配置历史版本；网关按接收顺序处理配置。
    // JSON 中必须显式提供，成员默认值不代表允许缺省字段。
    int schema_version{1};

    // 平台本次下发请求的唯一标识，必须非空，ACK 原样带回。
    // 同一次请求重发时保持不变，不使用遥测包的 message_id。
    std::string request_id;

    // 全部目标连接，connection_id 在本列表内必须唯一。
    // 显式 connections: [] 表示清空采集连接。
    // 缺少 connections、null 或非数组都应被解析器拒绝，
    // 不能因为本成员默认是空容器，就把缺少字段解释为清空配置。
    std::vector<CollectionConnectionConfig> connections;
};

} // namespace industrial_gateway::config
