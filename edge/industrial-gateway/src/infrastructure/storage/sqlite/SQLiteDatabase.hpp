#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>


namespace industrial_gateway::infrastructure::storage::sqlite
{

// ==================================================
// SQLite 数据类型
// ==================================================

// SQLite 支持的二进制数据
using SQLiteBlob = std::vector<std::uint8_t>;


// SQLite 参数和值类型
//
// std::monostate：NULL
// std::int64_t：整数
// double：浮点数
// std::string：文本
// SQLiteBlob：二进制数据

using SQLiteValue = std::variant<
    std::monostate,
    std::int64_t,
    double,
    std::string,
    SQLiteBlob
>;


// ==================================================
// SQLite 查询结果类型
// ==================================================

// 一行查询结果
using SQLiteRow = std::vector<SQLiteValue>;

// 多行查询结果
using SQLiteRows = std::vector<SQLiteRow>;


// ==================================================
// SQL 执行结果
// ==================================================

struct SQLiteExecuteResult
{
    // 当前数据库连接最近一次成功 INSERT 生成的 ROWID。
    // 注意：如果当前执行的不是 INSERT，该值可能仍是之前 INSERT 的 ROWID。
    std::int64_t last_insert_row_id = 0;

    // 本次 SQL 影响的行数
    int affected_rows = 0;
};


// ==================================================
// SQLite 数据库封装
// ==================================================
//
// 线程使用约定：
// 1. 一个 SQLiteDatabase 实例建议由一个工作线程独占使用。
// 2. 不同线程需要访问同一个数据库文件时，应分别创建 SQLiteDatabase 实例，
//    并分别打开同一个 database_path。
// 3. FULLMUTEX 只保证 SQLite 连接内部 API 调用的线程安全，不能保证跨线程
//    事务不会相互穿插。
//
// ==================================================

class SQLiteDatabase final
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    SQLiteDatabase();

    ~SQLiteDatabase();


    // 禁止拷贝
    SQLiteDatabase(const SQLiteDatabase&) = delete;

    SQLiteDatabase& operator=(
        const SQLiteDatabase&
    ) = delete;


    // 禁止移动
    SQLiteDatabase(SQLiteDatabase&&) = delete;

    SQLiteDatabase& operator=(
        SQLiteDatabase&&
    ) = delete;


    // ==================================================
    // 数据库连接管理
    // ==================================================

    // 打开 SQLite 数据库
    //
    // database_path: 数据库文件路径
    // busy_timeout_ms: 数据库忙等待超时时间（毫秒）
    // error_message: 操作失败时的错误信息
    //
    // 返回值：
    // true  - 打开成功
    // false - 打开失败
    bool open(
        const std::string& database_path,
        int busy_timeout_ms,
        std::string& error_message
    );


    // 关闭数据库
    void close() noexcept;


    // 检查数据库是否已经打开
    bool isOpen() const noexcept;


    // ==================================================
    // SQL 执行
    // ==================================================

    // 执行不需要参数绑定的 SQL
    //
    // 适用场景：
    // CREATE TABLE
    // CREATE INDEX
    // PRAGMA
    //
    // sql: SQL 语句
    // error_message: 操作失败时的错误信息
    //
    // 返回值：
    // true  - 执行成功
    // false - 执行失败
    bool executeRaw(
        const std::string& sql,
        std::string& error_message
    );


    // 执行带参数绑定的 SQL
    //
    // 适用场景：
    // INSERT
    // UPDATE
    // DELETE
    //
    // sql: SQL 语句
    // params: SQL 参数
    // result: SQL 执行结果；执行失败时会重置为空结果
    // error_message: 操作失败时的错误信息
    //
    // 返回值：
    // true  - 执行成功
    // false - 执行失败
    bool execute(
        const std::string& sql,
        const std::vector<SQLiteValue>& params,
        SQLiteExecuteResult& result,
        std::string& error_message
    );


    // ==================================================
    // 批量 SQL 执行
    // ==================================================

    // 使用同一条 SQL 执行多组参数
    //
    // sql: SQL 语句
    // parameter_sets: 多组 SQL 参数
    // affected_rows: 成功执行的参数组累计影响行数。
    //                如果中途失败，返回此前已成功执行部分的累计影响行数。
    // error_message: 操作失败时的错误信息
    //
    // 说明：
    // 1. 使用同一个预编译语句重复执行
    // 2. 空参数集合视为成功的空操作
    // 3. 不自动开启事务
    // 4. 如果需要批量操作具有原子性，调用者必须自行开启事务并在失败时回滚
    // 5. 如果调用者回滚事务，affected_rows 表示回滚前成功执行过的数量，
    //    不代表最终提交到数据库的数量
    //
    // 返回值：
    // true  - 全部执行成功
    // false - 至少一组参数执行失败
    bool executeBatch(
        const std::string& sql,
        const std::vector<std::vector<SQLiteValue>>& parameter_sets,
        int& affected_rows,
        std::string& error_message
    );


    // ==================================================
    // 数据查询
    // ==================================================

    // 执行查询 SQL
    //
    // sql: SQL 语句
    // params: SQL 参数
    // rows: 输出查询结果；查询开始前清空，失败时保持为空
    // error_message: 查询失败时的错误信息
    //
    // 返回值：
    // true  - 查询成功
    // false - 查询失败
    bool query(
        const std::string& sql,
        const std::vector<SQLiteValue>& params,
        SQLiteRows& rows,
        std::string& error_message
    );


    // ==================================================
    // 事务管理
    // ==================================================

    // 开启写事务
    //
    // 使用 BEGIN IMMEDIATE
    //
    // error_message: 操作失败时的错误信息
    //
    // 返回值：
    // true  - 开启成功
    // false - 开启失败
    bool beginTransaction(
        std::string& error_message
    );


    // 提交事务
    bool commit(
        std::string& error_message
    );


    // 回滚事务
    bool rollback(
        std::string& error_message
    );


private:

    // ==================================================
    // PImpl：隐藏 SQLite 具体实现
    // ==================================================

    class Impl;

    std::unique_ptr<Impl> impl_;

};

} // namespace industrial_gateway::infrastructure::storage::sqlite
