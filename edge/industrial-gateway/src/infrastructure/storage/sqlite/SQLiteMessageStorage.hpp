
#pragma once

#include "industrial_gateway/application/ports/IMessageStorage.hpp"
#include "industrial_gateway/config/StorageConfig.hpp"

#include "SQLiteDatabase.hpp"

#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::infrastructure::storage::sqlite
{

// ==================================================
// SQLite 消息存储实现
// ==================================================
//
// 实现 application::ports::IMessageStorage 接口。
//
// 核心职责：
// 1. 管理 SQLite 数据库连接。
// 2. 初始化 messages 表和索引。
// 3. 批量插入消息。
// 4. 批量查询待发送消息。
// 5. 批量删除已发送消息。
//
// 并发约定：
// 一个实例由一个工作线程独占使用。
// 不允许多个线程同时操作同一个实例。
//
// 不同线程可以创建不同实例，
// 连接同一个 SQLite 数据库文件。
//
// ==================================================

class SQLiteMessageStorage final
    : public application::ports::IMessageStorage
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    // 使用 SQLite 存储配置构造消息存储对象。
    //
    // config:
    // SQLite 数据库配置。
    //
    explicit SQLiteMessageStorage(
        const ::industrial_gateway::config::StorageConfig& config
    );

    ~SQLiteMessageStorage() override = default;


    // ==================================================
    // 禁止拷贝和移动
    // ==================================================

    SQLiteMessageStorage(
        const SQLiteMessageStorage&
    ) = delete;

    SQLiteMessageStorage& operator=(
        const SQLiteMessageStorage&
    ) = delete;

    SQLiteMessageStorage(
        SQLiteMessageStorage&&
    ) = delete;

    SQLiteMessageStorage& operator=(
        SQLiteMessageStorage&&
    ) = delete;


    // ==================================================
    // 初始化消息存储
    // ==================================================

    bool initialize(
        std::string& error_message
    ) override;


    // ==================================================
    // 批量插入消息
    // ==================================================

    bool insertBatch(
        const std::vector<application::ports::MessageToStore>& messages,
        std::string& error_message
    ) override;


    // ==================================================
    // 批量查询待发送消息
    // ==================================================

    bool queryBatch(
        std::size_t limit,
        std::vector<application::ports::StoredMessage>& messages,
        std::string& error_message
    ) override;


    // ==================================================
    // 批量删除已发送消息
    // ==================================================

    bool deleteBatch(
        const std::vector<std::int64_t>& ids,
        std::string& error_message
    ) override;


private:

    // ==================================================
    // 数据库连接
    // ==================================================

    // 当前存储实例独占的 SQLite 数据库连接。
    SQLiteDatabase database_;


    // ==================================================
    // 数据库配置
    // ==================================================

    // SQLite 数据库文件路径。
    std::string database_path_;

    // 数据库忙等待超时时间（毫秒）。
    int busy_timeout_ms_ = 5000;


    // ==================================================
    // 初始化状态
    // ==================================================

    // 是否已经成功完成数据库初始化。
    bool initialized_ = false;

};

} // namespace industrial_gateway::infrastructure::storage::sqlite
