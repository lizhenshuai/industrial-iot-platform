
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::application::ports
{

// ==================================================
// 待存储消息
// ==================================================
//
// 对应 messages 表中的：
// timestamp、message
//
// 数据库自动生成 id。
//
struct MessageToStore
{
    // 消息时间戳（UTC Unix 毫秒）
    std::int64_t timestamp_ms = 0;

    // 完整 MQTT 消息内容
    std::string message;
};


// ==================================================
// 已存储消息
// ==================================================
//
// 对应 messages 表中的：
// id、timestamp、message
//
// id 是 SQLite 自动生成的记录 ID，
// 用于后续删除已经成功发送的消息。
//
struct StoredMessage
{
    // SQLite 自增主键
    std::int64_t id = 0;

    // 消息时间戳（UTC Unix 毫秒）
    std::int64_t timestamp_ms = 0;

    // 完整 MQTT 消息内容
    std::string message;
};


// ==================================================
// 消息存储接口
// ==================================================
//
// 职责：
// 1. 初始化消息存储结构
// 2. 批量写入消息
// 3. 批量查询待发送消息
// 4. 批量删除已发送消息
//
// 不关心：
// 1. SQLite 底层实现
// 2. MQTT 连接和发布
// 3. 采集数据的产生
// 4. 消息 JSON 的具体结构
//
// ==================================================

class IMessageStorage
{
public:

    // ==================================================
    // 析构函数
    // ==================================================

    virtual ~IMessageStorage() = default;


    // ==================================================
    // 初始化消息存储
    // ==================================================

    // 初始化消息存储结构。
    //
    // 负责：
    // 1. 创建 messages 表
    // 2. 创建 idx_timestamp 索引
    //
    // 重复初始化应该安全，不应该重复创建或报错。
    //
    // error_message:
    // 初始化失败时返回错误信息。
    //
    // 返回值：
    // true  - 初始化成功
    // false - 初始化失败
    //
    virtual bool initialize(
        std::string& error_message
    ) = 0;


    // ==================================================
    // 批量插入消息
    // ==================================================

    // 批量写入消息。
    //
    // 每批建议 100 条。
    //
    // 实现要求：
    // 1. 空批次作为成功的空操作。
    // 2. 一批数据必须在同一个事务内完成。
    // 3. 全部写入成功才提交事务。
    // 4. 任意一条写入失败，回滚整个批次。
    //
    // messages:
    // 待写入的消息集合。
    //
    // error_message:
    // 写入失败时返回错误信息。
    //
    // 返回值：
    // true  - 整批写入成功
    // false - 整批写入失败
    //
    virtual bool insertBatch(
        const std::vector<MessageToStore>& messages,
        std::string& error_message
    ) = 0;


    // ==================================================
    // 批量查询待发送消息
    // ==================================================

    // 按照 SQLite 自增 ID 升序查询消息。
    //
    // 实现要求：
    // 1. 按照 id ASC 排序。
    // 2. 最多返回 limit 条记录。
    // 3. 不删除查询到的消息。
    // 4. 查询失败时清空输出集合。
    //
    // limit:
    // 本次最多查询的消息数量。
    //
    // messages:
    // 输出查询到的消息。
    //
    // error_message:
    // 查询失败时返回错误信息。
    //
    // 返回值：
    // true  - 查询成功
    // false - 查询失败
    //
    virtual bool queryBatch(
        std::size_t limit,
        std::vector<StoredMessage>& messages,
        std::string& error_message
    ) = 0;


    // ==================================================
    // 批量删除已发送消息
    // ==================================================

    // 根据 SQLite 自增 ID 批量删除消息。
    //
    // 实现要求：
    // 1. 根据 id 删除指定消息。
    // 2. 一批删除操作使用同一个事务。
    // 3. 全部删除成功才提交事务。
    // 4. 任意一条删除失败，回滚整个批次。
    // 5. 空 ID 集合作为成功的空操作。
    //
    // ids:
    // 需要删除的 SQLite 记录 ID。
    //
    // error_message:
    // 删除失败时返回错误信息。
    //
    // 返回值：
    // true  - 删除成功
    // false - 删除失败
    //
    virtual bool deleteBatch(
        const std::vector<std::int64_t>& ids,
        std::string& error_message
    ) = 0;

};

} // namespace industrial_gateway::application::ports
