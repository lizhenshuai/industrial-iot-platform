#include "SQLiteMessageStorage.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>


namespace industrial_gateway::infrastructure::storage::sqlite
{

namespace
{

// ==================================================
// 事务回滚辅助函数
// ==================================================
//
// 操作失败后尝试回滚事务。
//
// 处理规则：
// 1. 回滚成功：保持数据库连接和初始化状态。
// 2. 回滚失败：
//    - 追加回滚错误。
//    - 关闭数据库连接。
//    - 将初始化状态设置为 false。
//    - 检查连接是否成功关闭。
//
// 注意：
// SQLiteDatabase::close() 不返回关闭状态，
// 因此需要通过 isOpen() 检查关闭结果。
//
// ==================================================

void rollbackAndInvalidateOnFailure(
    SQLiteDatabase& database,
    bool& initialized,
    std::string& error_message
)
{
    std::string rollback_error;

    // 尝试回滚事务
    if (database.rollback(rollback_error))
    {
        return;
    }


    // --------------------------------------------------
    // 1. 记录回滚错误
    // --------------------------------------------------

    if (!error_message.empty())
    {
        error_message += "；";
    }

    error_message +=
        "事务回滚失败: " + rollback_error;


    // --------------------------------------------------
    // 2. 关闭数据库连接
    // --------------------------------------------------

    database.close();


    // --------------------------------------------------
    // 3. 重置初始化状态
    // --------------------------------------------------

    initialized = false;


    // --------------------------------------------------
    // 4. 检查连接是否成功关闭
    // --------------------------------------------------

    if (database.isOpen())
    {
        error_message +=
            "；关闭 SQLite 数据库连接失败，"
            "连接仍处于打开状态";
    }
}


// ==================================================
// 创建数据库父目录
// ==================================================

bool ensureDatabaseDirectory(
    const std::string& database_path,
    std::string& error_message
)
{
    const std::filesystem::path path(database_path);

    const std::filesystem::path parent_path =
        path.parent_path();

    // 没有父目录，例如 gateway.db
    if (parent_path.empty())
    {
        return true;
    }

    std::error_code ec;

    std::filesystem::create_directories(
        parent_path,
        ec
    );

    if (ec)
    {
        error_message =
            "创建 SQLite 数据库目录失败: " +
            ec.message();

        return false;
    }

    return true;
}

} // namespace


// ==================================================
// 构造函数
// ==================================================

SQLiteMessageStorage::SQLiteMessageStorage(
    const ::industrial_gateway::config::StorageConfig& config
)
    : database_path_(config.database_path),
      busy_timeout_ms_(config.busy_timeout_ms)
{
}


// ==================================================
// 初始化消息存储
// ==================================================

bool SQLiteMessageStorage::initialize(
    std::string& error_message
)
{
    error_message.clear();


    // ==================================================
    // 1. 检查初始化状态
    // ==================================================

    // 已经初始化，直接返回成功
    if (initialized_)
    {
        return true;
    }


    // ==================================================
    // 2. 检查并清理上一次的数据库连接
    // ==================================================

    // 如果之前初始化失败，可能留下一个仍然打开的连接。
    //
    // 为了避免复用状态不确定的旧连接，
    // 在重新初始化前先尝试关闭它。

    if (database_.isOpen())
    {
        database_.close();

        // close() 不提供返回值，通过 isOpen() 判断结果。
        if (database_.isOpen())
        {
            error_message =
                "上一次 SQLite 数据库连接关闭失败，"
                "无法安全重新初始化";

            return false;
        }
    }


    // ==================================================
    // 3. 创建数据库父目录
    // ==================================================

    if (
        !ensureDatabaseDirectory(
            database_path_,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 4. 打开数据库
    // ==================================================

    if (!database_.isOpen())
    {
        if (
            !database_.open(
                database_path_,
                busy_timeout_ms_,
                error_message
            )
        )
        {
            return false;
        }
    }


    // ==================================================
    // 5. 开启事务
    // ==================================================

    if (!database_.beginTransaction(error_message))
    {
        return false;
    }


    // ==================================================
    // 6. 创建 messages 表
    // ==================================================

    const std::string create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            message TEXT NOT NULL
        );
    )";

    if (
        !database_.executeRaw(
            create_table_sql,
            error_message
        )
    )
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }


    // ==================================================
    // 7. 创建时间戳索引
    // ==================================================

    const std::string create_index_sql = R"(
        CREATE INDEX IF NOT EXISTS idx_timestamp
        ON messages (timestamp);
    )";

    if (
        !database_.executeRaw(
            create_index_sql,
            error_message
        )
    )
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }


    // ==================================================
    // 8. 提交事务
    // ==================================================

    if (!database_.commit(error_message))
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }


    // ==================================================
    // 9. 初始化完成
    // ==================================================

    initialized_ = true;

    return true;
}


// ==================================================
// 批量插入消息
// ==================================================

bool SQLiteMessageStorage::insertBatch(
    const std::vector<application::ports::MessageToStore>& messages,
    std::string& error_message
)
{
    error_message.clear();


    // ==================================================
    // 1. 空批次处理
    // ==================================================

    if (messages.empty())
    {
        return true;
    }


    // ==================================================
    // 2. 检查初始化状态
    // ==================================================

    if (!initialized_ || !database_.isOpen())
    {
        error_message =
            "SQLite 消息存储尚未初始化";

        return false;
    }


    // ==================================================
    // 3. 准备批量 SQL 参数
    // ==================================================

    const std::string sql = R"(
        INSERT INTO messages (
            timestamp,
            message
        )
        VALUES (?, ?);
    )";

    std::vector<std::vector<SQLiteValue>> parameter_sets;

    parameter_sets.reserve(messages.size());

    for (const auto& message : messages)
    {
        parameter_sets.push_back({
            message.timestamp_ms,
            message.message
        });
    }


    // ==================================================
    // 4. 开启事务
    // ==================================================

    if (!database_.beginTransaction(error_message))
    {
        return false;
    }


    // ==================================================
    // 5. 执行批量插入
    // ==================================================

    int affected_rows = 0;

    if (
        !database_.executeBatch(
            sql,
            parameter_sets,
            affected_rows,
            error_message
        )
    )
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }


    // ==================================================
    // 6. 提交事务
    // ==================================================

    if (!database_.commit(error_message))
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }

    return true;
}


// ==================================================
// 批量查询待发送消息
// ==================================================

bool SQLiteMessageStorage::queryBatch(
    std::size_t limit,
    std::vector<application::ports::StoredMessage>& messages,
    std::string& error_message
)
{
    error_message.clear();

    messages.clear();


    // ==================================================
    // 1. 参数校验
    // ==================================================

    if (limit == 0)
    {
        error_message =
            "SQLite 消息查询数量必须大于 0";

        return false;
    }

    if (
        static_cast<std::uintmax_t>(limit) >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::int64_t>::max()
        )
    )
    {
        error_message =
            "SQLite 消息查询数量超出 int64_t 表示范围";

        return false;
    }


    // ==================================================
    // 2. 检查初始化状态
    // ==================================================

    if (!initialized_ || !database_.isOpen())
    {
        error_message =
            "SQLite 消息存储尚未初始化";

        return false;
    }


    // ==================================================
    // 3. 执行查询
    // ==================================================

    const std::string sql = R"(
        SELECT
            id,
            timestamp,
            message
        FROM messages
        ORDER BY id ASC
        LIMIT ?;
    )";

    SQLiteRows rows;

    if (
        !database_.query(
            sql,
            {
                static_cast<std::int64_t>(limit)
            },
            rows,
            error_message
        )
    )
    {
        return false;
    }


    // ==================================================
    // 4. 转换查询结果
    // ==================================================

    messages.reserve(rows.size());

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& row = rows[i];

        // 检查列数量
        if (row.size() != 3)
        {
            error_message =
                "SQLite 消息查询结果列数量异常";

            messages.clear();

            return false;
        }


        // 读取 id
        const auto* id =
            std::get_if<std::int64_t>(&row[0]);


        // 读取 timestamp
        const auto* timestamp =
            std::get_if<std::int64_t>(&row[1]);


        // 读取 message
        const auto* message =
            std::get_if<std::string>(&row[2]);


        // 检查数据类型
        if (
            id == nullptr ||
            timestamp == nullptr ||
            message == nullptr
        )
        {
            error_message =
                "SQLite 消息查询结果数据类型异常，行号: " +
                std::to_string(i);

            messages.clear();

            return false;
        }


        // 构造消息对象
        application::ports::StoredMessage stored_message;

        stored_message.id = *id;

        stored_message.timestamp_ms = *timestamp;

        stored_message.message = *message;

        messages.push_back(
            std::move(stored_message)
        );
    }

    return true;
}


// ==================================================
// 批量删除已发送消息
// ==================================================

bool SQLiteMessageStorage::deleteBatch(
    const std::vector<std::int64_t>& ids,
    std::string& error_message
)
{
    error_message.clear();


    // ==================================================
    // 1. 空批次处理
    // ==================================================

    if (ids.empty())
    {
        return true;
    }


    // ==================================================
    // 2. 检查初始化状态
    // ==================================================

    if (!initialized_ || !database_.isOpen())
    {
        error_message =
            "SQLite 消息存储尚未初始化";

        return false;
    }


    // ==================================================
    // 3. 准备批量 SQL 参数
    // ==================================================

    const std::string sql = R"(
        DELETE FROM messages
        WHERE id = ?;
    )";

    std::vector<std::vector<SQLiteValue>> parameter_sets;

    parameter_sets.reserve(ids.size());

    for (const auto id : ids)
    {
        parameter_sets.push_back({
            id
        });
    }


    // ==================================================
    // 4. 开启事务
    // ==================================================

    if (!database_.beginTransaction(error_message))
    {
        return false;
    }


    // ==================================================
    // 5. 执行批量删除
    // ==================================================

    int affected_rows = 0;

    if (
        !database_.executeBatch(
            sql,
            parameter_sets,
            affected_rows,
            error_message
        )
    )
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }


    // ==================================================
    // 6. 提交事务
    // ==================================================

    if (!database_.commit(error_message))
    {
        rollbackAndInvalidateOnFailure(
            database_,
            initialized_,
            error_message
        );

        return false;
    }

    return true;
}

} // namespace industrial_gateway::infrastructure::storage::sqlite