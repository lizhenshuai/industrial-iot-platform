
#include "SQLiteDatabase.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>


namespace fs = std::filesystem;

namespace sqlite_storage =
    industrial_gateway::infrastructure::storage::sqlite;

using sqlite_storage::SQLiteDatabase;
using sqlite_storage::SQLiteValue;
using sqlite_storage::SQLiteBlob;
using sqlite_storage::SQLiteRow;
using sqlite_storage::SQLiteRows;
using sqlite_storage::SQLiteExecuteResult;


// ==================================================
// SQLite 数据库测试夹具
// ==================================================

class SQLiteDatabaseTest : public ::testing::Test
{
protected:

    // ==================================================
    // 测试初始化
    // ==================================================

    void SetUp() override
    {
        static std::atomic<std::uint64_t> sequence{0};

        const auto timestamp =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        const auto unique_id = sequence.fetch_add(1);

        test_directory_ =
            fs::temp_directory_path() /
            "industrial_gateway_sqlite_tests" /
            (
                "test_" +
                std::to_string(timestamp) +
                "_" +
                std::to_string(unique_id)
            );

        std::error_code ec;

        fs::create_directories(test_directory_, ec);

        ASSERT_FALSE(ec)
            << "创建测试目录失败: " << ec.message();

        database_path_ = test_directory_ / "gateway.db";
    }


    // ==================================================
    // 测试清理
    // ==================================================

    void TearDown() override
    {
        database_.close();

        if (!test_directory_.empty())
        {
            std::error_code ec;

            fs::remove_all(test_directory_, ec);

            EXPECT_FALSE(ec)
                << "清理测试目录失败: " << ec.message();
        }
    }


    // ==================================================
    // 打开数据库
    // ==================================================

    bool openDatabase(int busy_timeout_ms = 5000)
    {
        error_message_.clear();

        return database_.open(
            database_path_.string(),
            busy_timeout_ms,
            error_message_
        );
    }


    // ==================================================
    // 创建基础测试表
    // ==================================================

    bool createRecordsTable()
    {
        return database_.executeRaw(
            R"(
                CREATE TABLE IF NOT EXISTS records (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp INTEGER NOT NULL,
                    message TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_records_timestamp
                ON records(timestamp);
            )",
            error_message_
        );
    }


    // ==================================================
    // 插入一条测试记录
    // ==================================================

    bool insertRecord(
        std::int64_t timestamp,
        const std::string& message
    )
    {
        SQLiteExecuteResult result;

        return database_.execute(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            {
                timestamp,
                message
            },
            result,
            error_message_
        );
    }


    // ==================================================
    // 查询单个整数结果
    // ==================================================

    bool querySingleInt64(
        SQLiteDatabase& database,
        const std::string& sql,
        const std::vector<SQLiteValue>& params,
        std::int64_t& value,
        std::string& error_message
    )
    {
        SQLiteRows rows;

        if (!database.query(
                sql,
                params,
                rows,
                error_message
            ))
        {
            return false;
        }

        if (rows.empty() || rows.front().empty())
        {
            error_message = "查询结果为空";
            return false;
        }

        const auto* result =
            std::get_if<std::int64_t>(&rows.front().front());

        if (result == nullptr)
        {
            error_message = "查询结果不是 INTEGER 类型";
            return false;
        }

        value = *result;

        return true;
    }


protected:

    SQLiteDatabase database_;

    fs::path test_directory_;

    fs::path database_path_;

    std::string error_message_;
};


// ==================================================
// 第一组：数据库连接管理测试
// ==================================================


// 初始状态、打开和关闭
TEST_F(
    SQLiteDatabaseTest,
    OpenCloseAndBasicState
)
{
    EXPECT_FALSE(database_.isOpen());

    ASSERT_TRUE(openDatabase())
        << error_message_;

    EXPECT_TRUE(database_.isOpen());

    database_.close();

    EXPECT_FALSE(database_.isOpen());

    // 重复关闭应该安全
    database_.close();

    EXPECT_FALSE(database_.isOpen());
}


// 空数据库路径和负数超时时间
TEST_F(
    SQLiteDatabaseTest,
    RejectInvalidOpenArguments
)
{
    EXPECT_FALSE(
        database_.open(
            "",
            5000,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_FALSE(database_.isOpen());


    error_message_.clear();

    EXPECT_FALSE(
        database_.open(
            database_path_.string(),
            -1,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_FALSE(database_.isOpen());
}


// 不允许重复打开
TEST_F(
    SQLiteDatabaseTest,
    RejectRepeatedOpen
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    EXPECT_FALSE(
        database_.open(
            database_path_.string(),
            5000,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_TRUE(database_.isOpen());
}


// 数据库父目录不存在时打开失败
TEST_F(
    SQLiteDatabaseTest,
    RejectOpenWhenParentDirectoryDoesNotExist
)
{
    const fs::path invalid_path =
        test_directory_ / "missing" / "gateway.db";

    EXPECT_FALSE(
        database_.open(
            invalid_path.string(),
            5000,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_FALSE(database_.isOpen());

    EXPECT_FALSE(fs::exists(invalid_path));
}


// ==================================================
// 第二组：数据库配置测试
// ==================================================


// 验证 WAL、FULL、外键和 busy_timeout
TEST_F(
    SQLiteDatabaseTest,
    VerifyDatabasePragmaSettings
)
{
    ASSERT_TRUE(openDatabase(1234))
        << error_message_;

    SQLiteRows rows;


    // --------------------------------------------------
    // 1. journal_mode = WAL
    // --------------------------------------------------

    ASSERT_TRUE(
        database_.query(
            "PRAGMA journal_mode;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    const auto* journal_mode =
        std::get_if<std::string>(&rows[0][0]);

    ASSERT_NE(journal_mode, nullptr);

    EXPECT_EQ(*journal_mode, "wal");


    // --------------------------------------------------
    // 2. synchronous = FULL
    // --------------------------------------------------

    rows.clear();

    ASSERT_TRUE(
        database_.query(
            "PRAGMA synchronous;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    ASSERT_TRUE(
        std::holds_alternative<std::int64_t>(rows[0][0])
    );

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        2
    );


    // --------------------------------------------------
    // 3. foreign_keys = ON
    // --------------------------------------------------

    rows.clear();

    ASSERT_TRUE(
        database_.query(
            "PRAGMA foreign_keys;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        1
    );


    // --------------------------------------------------
    // 4. busy_timeout
    // --------------------------------------------------

    rows.clear();

    ASSERT_TRUE(
        database_.query(
            "PRAGMA busy_timeout;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        1234
    );
}


// ==================================================
// 第三组：SQL 执行测试
// ==================================================


// 执行原始 SQL
TEST_F(
    SQLiteDatabaseTest,
    ExecuteRawCreateTableAndIndex
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?;",
            {
                std::string("records")
            },
            rows,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(rows.size(), 1U);
}


// 参数化插入和执行结果
TEST_F(
    SQLiteDatabaseTest,
    ExecuteInsertAndCheckResult
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    SQLiteExecuteResult result;

    ASSERT_TRUE(
        database_.execute(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            {
                std::int64_t{1000},
                std::string("hello sqlite")
            },
            result,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(result.last_insert_row_id, 1);

    EXPECT_EQ(result.affected_rows, 1);
}


// 参数数量不匹配
TEST_F(
    SQLiteDatabaseTest,
    RejectParameterCountMismatch
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    SQLiteExecuteResult result;

    result.last_insert_row_id = 999;
    result.affected_rows = 999;

    EXPECT_FALSE(
        database_.execute(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            {
                std::int64_t{1000}
            },
            result,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    // 失败后不保留旧执行结果
    EXPECT_EQ(result.last_insert_row_id, 0);

    EXPECT_EQ(result.affected_rows, 0);
}


// ==================================================
// 第四组：SQLite 数据类型测试
// ==================================================


// INTEGER / FLOAT / TEXT / BLOB / NULL
TEST_F(
    SQLiteDatabaseTest,
    SupportAllSQLiteValueTypes
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(
        database_.executeRaw(
            R"(
                CREATE TABLE typed_values (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    int_value INTEGER,
                    real_value REAL,
                    text_value TEXT,
                    blob_value BLOB,
                    null_value TEXT
                );
            )",
            error_message_
        )
    ) << error_message_;


    SQLiteBlob expected_blob = {
        0,
        1,
        2,
        255
    };


    // --------------------------------------------------
    // 插入完整数据类型
    // --------------------------------------------------

    SQLiteExecuteResult result;

    ASSERT_TRUE(
        database_.execute(
            R"(
                INSERT INTO typed_values (
                    int_value,
                    real_value,
                    text_value,
                    blob_value,
                    null_value
                )
                VALUES (?, ?, ?, ?, ?);
            )",
            {
                std::int64_t{42},
                36.5,
                std::string("temperature"),
                expected_blob,
                std::monostate{}
            },
            result,
            error_message_
        )
    ) << error_message_;


    // --------------------------------------------------
    // 查询数据
    // --------------------------------------------------

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            R"(
                SELECT
                    int_value,
                    real_value,
                    text_value,
                    blob_value,
                    null_value
                FROM typed_values
                WHERE int_value = ?;
            )",
            {
                std::int64_t{42}
            },
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    ASSERT_EQ(rows[0].size(), 5U);


    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        42
    );

    EXPECT_DOUBLE_EQ(
        std::get<double>(rows[0][1]),
        36.5
    );

    EXPECT_EQ(
        std::get<std::string>(rows[0][2]),
        "temperature"
    );

    EXPECT_EQ(
        std::get<SQLiteBlob>(rows[0][3]),
        expected_blob
    );

    EXPECT_TRUE(
        std::holds_alternative<std::monostate>(rows[0][4])
    );
}


// 空字符串和空 BLOB 不应该被识别为 NULL
TEST_F(
    SQLiteDatabaseTest,
    DistinguishEmptyTextEmptyBlobAndNull
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(
        database_.executeRaw(
            R"(
                CREATE TABLE empty_values (
                    id INTEGER PRIMARY KEY,
                    text_value TEXT,
                    blob_value BLOB,
                    null_value TEXT
                );
            )",
            error_message_
        )
    ) << error_message_;

    SQLiteExecuteResult result;

    ASSERT_TRUE(
        database_.execute(
            R"(
                INSERT INTO empty_values
                VALUES (?, ?, ?, ?);
            )",
            {
                std::int64_t{1},
                std::string{},
                SQLiteBlob{},
                std::monostate{}
            },
            result,
            error_message_
        )
    ) << error_message_;

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            "SELECT text_value, blob_value, null_value FROM empty_values;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_TRUE(
        std::holds_alternative<std::string>(rows[0][0])
    );

    EXPECT_TRUE(
        std::get<std::string>(rows[0][0]).empty()
    );

    EXPECT_TRUE(
        std::holds_alternative<SQLiteBlob>(rows[0][1])
    );

    EXPECT_TRUE(
        std::get<SQLiteBlob>(rows[0][1]).empty()
    );

    EXPECT_TRUE(
        std::holds_alternative<std::monostate>(rows[0][2])
    );
}


// ==================================================
// 第五组：批量执行测试
// ==================================================


// 一批插入 100 条数据
TEST_F(
    SQLiteDatabaseTest,
    ExecuteBatchInsert100Records
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    constexpr int kBatchSize = 100;

    std::vector<std::vector<SQLiteValue>> parameter_sets;

    parameter_sets.reserve(kBatchSize);

    for (int i = 0; i < kBatchSize; ++i)
    {
        parameter_sets.push_back(
            {
                std::int64_t{1000 + i},
                std::string("message_") + std::to_string(i)
            }
        );
    }

    int affected_rows = 0;

    ASSERT_TRUE(
        database_.beginTransaction(error_message_)
    ) << error_message_;

    if (!database_.executeBatch(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            parameter_sets,
            affected_rows,
            error_message_
        ))
    {
        const std::string original_error = error_message_;

        std::string rollback_error;

        database_.rollback(rollback_error);

        FAIL() << original_error;
    }

    ASSERT_TRUE(
        database_.commit(error_message_)
    ) << error_message_;

    EXPECT_EQ(affected_rows, 100);


    std::int64_t count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM records;",
            {},
            count,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(count, 100);
}


// 空批次应该成功
TEST_F(
    SQLiteDatabaseTest,
    ExecuteBatchEmptyParameters
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    int affected_rows = 100;

    EXPECT_TRUE(
        database_.executeBatch(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            {},
            affected_rows,
            error_message_
        )
    );

    EXPECT_EQ(affected_rows, 0);
}


// 批量执行部分失败时，返回已执行数量
TEST_F(
    SQLiteDatabaseTest,
    ExecuteBatchReportsPartialSuccess
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(
        database_.executeRaw(
            R"(
                CREATE TABLE unique_values (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    value TEXT NOT NULL UNIQUE
                );
            )",
            error_message_
        )
    ) << error_message_;

    std::vector<std::vector<SQLiteValue>> parameter_sets;

    parameter_sets.push_back({std::string("alpha")});

    parameter_sets.push_back({std::string("beta")});

    parameter_sets.push_back({std::string("alpha")});

    int affected_rows = 0;

    EXPECT_FALSE(
        database_.executeBatch(
            "INSERT INTO unique_values (value) VALUES (?);",
            parameter_sets,
            affected_rows,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_EQ(affected_rows, 2);

    std::int64_t count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM unique_values;",
            {},
            count,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(count, 2);
}


// ==================================================
// 第六组：查询测试
// ==================================================


// 正常查询
TEST_F(
    SQLiteDatabaseTest,
    QueryReturnsCorrectResults
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    ASSERT_TRUE(insertRecord(1000, "temperature"))
        << error_message_;

    ASSERT_TRUE(insertRecord(2000, "speed"))
        << error_message_;

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            "SELECT timestamp, message FROM records ORDER BY id;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 2U);

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        1000
    );

    EXPECT_EQ(
        std::get<std::string>(rows[0][1]),
        "temperature"
    );

    EXPECT_EQ(
        std::get<std::int64_t>(rows[1][0]),
        2000
    );

    EXPECT_EQ(
        std::get<std::string>(rows[1][1]),
        "speed"
    );
}


// 没有数据时查询成功，结果为空
TEST_F(
    SQLiteDatabaseTest,
    QueryEmptyResult
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            "SELECT * FROM records WHERE id = ?;",
            {
                std::int64_t{999}
            },
            rows,
            error_message_
        )
    ) << error_message_;

    EXPECT_TRUE(rows.empty());
}


// 查询失败时清空旧结果
TEST_F(
    SQLiteDatabaseTest,
    QueryFailureClearsPreviousResults
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    SQLiteRows rows;

    rows.push_back({
        std::int64_t{999}
    });

    EXPECT_FALSE(
        database_.query(
            "SELECT FROM invalid_table;",
            {},
            rows,
            error_message_
        )
    );

    EXPECT_TRUE(rows.empty());

    EXPECT_FALSE(error_message_.empty());
}


// ==================================================
// 第七组：事务测试
// ==================================================


// COMMIT 和 ROLLBACK
TEST_F(
    SQLiteDatabaseTest,
    TransactionCommitAndRollback
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;


    // --------------------------------------------------
    // 第一笔事务：提交
    // --------------------------------------------------

    ASSERT_TRUE(
        database_.beginTransaction(error_message_)
    ) << error_message_;

    ASSERT_TRUE(
        insertRecord(1000, "commit_record")
    ) << error_message_;

    ASSERT_TRUE(
        database_.commit(error_message_)
    ) << error_message_;


    std::int64_t count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM records;",
            {},
            count,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(count, 1);


    // --------------------------------------------------
    // 第二笔事务：回滚
    // --------------------------------------------------

    ASSERT_TRUE(
        database_.beginTransaction(error_message_)
    ) << error_message_;

    ASSERT_TRUE(
        insertRecord(2000, "rollback_record")
    ) << error_message_;

    ASSERT_TRUE(
        database_.rollback(error_message_)
    ) << error_message_;


    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM records;",
            {},
            count,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(count, 1);
}


// 批量操作失败后，通过事务回滚全部数据
TEST_F(
    SQLiteDatabaseTest,
    TransactionRollbackAfterBatchFailure
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(
        database_.executeRaw(
            R"(
                CREATE TABLE unique_values (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    value TEXT NOT NULL UNIQUE
                );
            )",
            error_message_
        )
    ) << error_message_;

    ASSERT_TRUE(
        database_.beginTransaction(error_message_)
    ) << error_message_;

    std::vector<std::vector<SQLiteValue>> parameter_sets;

    parameter_sets.push_back({std::string("alpha")});

    parameter_sets.push_back({std::string("beta")});

    parameter_sets.push_back({std::string("alpha")});

    int affected_rows = 0;

    EXPECT_FALSE(
        database_.executeBatch(
            "INSERT INTO unique_values (value) VALUES (?);",
            parameter_sets,
            affected_rows,
            error_message_
        )
    );

    EXPECT_EQ(affected_rows, 2);

    ASSERT_TRUE(
        database_.rollback(error_message_)
    ) << error_message_;

    std::int64_t count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM unique_values;",
            {},
            count,
            error_message_
        )
    ) << error_message_;

    EXPECT_EQ(count, 0);
}


// ==================================================
// 第八组：数据库持久化测试
// ==================================================


// 关闭后重新打开，数据仍然存在
TEST_F(
    SQLiteDatabaseTest,
    DataPersistsAfterCloseAndReopen
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;

    ASSERT_TRUE(insertRecord(1000, "persistent_message"))
        << error_message_;

    database_.close();

    EXPECT_FALSE(database_.isOpen());

    ASSERT_TRUE(openDatabase())
        << error_message_;

    EXPECT_TRUE(database_.isOpen());

    SQLiteRows rows;

    ASSERT_TRUE(
        database_.query(
            "SELECT timestamp, message FROM records;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        1000
    );

    EXPECT_EQ(
        std::get<std::string>(rows[0][1]),
        "persistent_message"
    );
}


// ==================================================
// 第九组：独立连接测试
// ==================================================


// 两个数据库对象分别连接同一个数据库文件
TEST_F(
    SQLiteDatabaseTest,
    MultipleConnectionsAccessSameDatabase
)
{
    ASSERT_TRUE(openDatabase())
        << error_message_;

    ASSERT_TRUE(createRecordsTable())
        << error_message_;


    SQLiteDatabase second_database;

    std::string second_error;

    ASSERT_TRUE(
        second_database.open(
            database_path_.string(),
            5000,
            second_error
        )
    ) << second_error;


    // 第一个连接写入
    ASSERT_TRUE(
        insertRecord(1000, "connection_one")
    ) << error_message_;


    // 第二个连接读取
    SQLiteRows rows;

    ASSERT_TRUE(
        second_database.query(
            "SELECT timestamp, message FROM records;",
            {},
            rows,
            second_error
        )
    ) << second_error;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(
        std::get<std::string>(rows[0][1]),
        "connection_one"
    );


    // 第二个连接写入
    SQLiteExecuteResult result;

    ASSERT_TRUE(
        second_database.execute(
            "INSERT INTO records (timestamp, message) VALUES (?, ?);",
            {
                std::int64_t{2000},
                std::string("connection_two")
            },
            result,
            second_error
        )
    ) << second_error;


    // 第一个连接读取
    rows.clear();

    ASSERT_TRUE(
        database_.query(
            "SELECT COUNT(*) FROM records;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    EXPECT_EQ(
        std::get<std::int64_t>(rows[0][0]),
        2
    );

    second_database.close();
}


// ==================================================
// 第十组：关闭状态操作测试
// ==================================================

TEST_F(
    SQLiteDatabaseTest,
    RejectOperationsWhenDatabaseIsClosed
)
{
    SQLiteExecuteResult result;

    int affected_rows = 0;

    SQLiteRows rows;

    // executeRaw
    EXPECT_FALSE(
        database_.executeRaw(
            "SELECT 1;",
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // execute
    error_message_.clear();

    EXPECT_FALSE(
        database_.execute(
            "INSERT INTO records VALUES (?, ?);",
            {
                std::int64_t{1},
                std::string("test")
            },
            result,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // executeBatch
    error_message_.clear();

    EXPECT_FALSE(
        database_.executeBatch(
            "INSERT INTO records VALUES (?, ?);",
            {
                {
                    std::int64_t{1},
                    std::string("test")
                }
            },
            affected_rows,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // query
    error_message_.clear();

    EXPECT_FALSE(
        database_.query(
            "SELECT * FROM records;",
            {},
            rows,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // beginTransaction
    error_message_.clear();

    EXPECT_FALSE(
        database_.beginTransaction(error_message_)
    );

    EXPECT_FALSE(error_message_.empty());


    // commit
    error_message_.clear();

    EXPECT_FALSE(
        database_.commit(error_message_)
    );

    EXPECT_FALSE(error_message_.empty());


    // rollback
    error_message_.clear();

    EXPECT_FALSE(
        database_.rollback(error_message_)
    );

    EXPECT_FALSE(error_message_.empty());
}


// ==================================================
// 第十一组：性能测试
// ==================================================

class SQLiteDatabasePerformanceTest
    : public SQLiteDatabaseTest
{
};


// ==================================================
// 100 万条数据批量插入 + 完整查询
// ==================================================

TEST_F(
    SQLiteDatabasePerformanceTest,
    InsertOneMillionRecordsInBatchesOf100AndQueryAll
)
{
    // 性能测试需要显式开启，避免每次运行功能测试
    // 都执行 100 万条数据的性能测试。
    if (std::getenv("RUN_SQLITE_PERFORMANCE_TESTS") == nullptr)
    {
        GTEST_SKIP()
            << "设置 RUN_SQLITE_PERFORMANCE_TESTS=1 "
            << "以运行 100 万条数据性能测试";
    }


    ASSERT_TRUE(openDatabase())
        << error_message_;


    // ==================================================
    // 1. 创建正式消息表
    // ==================================================

    ASSERT_TRUE(
        database_.executeRaw(
            R"(
                CREATE TABLE IF NOT EXISTS messages (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp INTEGER NOT NULL,
                    message TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_timestamp
                ON messages(timestamp);
            )",
            error_message_
        )
    ) << error_message_;


    // ==================================================
    // 2. 性能测试参数
    // ==================================================

    constexpr std::int64_t kTotalRecords = 1'000'000;

    constexpr std::size_t kBatchSize = 100;

    constexpr std::int64_t kBatchCount =
        kTotalRecords / static_cast<std::int64_t>(kBatchSize);

    constexpr std::int64_t kBaseTimestamp =
        1'700'000'000'000LL;


    const std::string insert_sql =
        "INSERT INTO messages (timestamp, message) VALUES (?, ?);";


    // ==================================================
    // 3. 批量插入
    // ==================================================

    using Clock = std::chrono::steady_clock;

    double database_operation_ms = 0.0;

    const auto insert_start = Clock::now();

    std::int64_t total_inserted = 0;

    for (std::int64_t batch_index = 0;
         batch_index < kBatchCount;
         ++batch_index)
    {
        std::vector<std::vector<SQLiteValue>> parameter_sets;

        parameter_sets.reserve(kBatchSize);


        // --------------------------------------------------
        // 构造当前批次 100 条数据
        // --------------------------------------------------

        const std::int64_t first_record =
            batch_index * static_cast<std::int64_t>(kBatchSize);

        for (std::size_t i = 0; i < kBatchSize; ++i)
        {
            const std::int64_t sequence =
                first_record + static_cast<std::int64_t>(i) + 1;

            const std::string message =
                "{\"message_id\":\"msg-" +
                std::to_string(sequence) +
                "\",\"device_id\":\"PLC001\","
                "\"property\":\"temperature\","
                "\"value\":36.5,\"status\":\"Success\"}";

            parameter_sets.push_back(
                {
                    kBaseTimestamp + sequence,
                    message
                }
            );
        }


        // --------------------------------------------------
        // 每批独立事务
        // --------------------------------------------------

        const auto database_start = Clock::now();

        if (!database_.beginTransaction(error_message_))
        {
            FAIL() << "开启事务失败: " << error_message_;
        }


        int affected_rows = 0;

        if (!database_.executeBatch(
                insert_sql,
                parameter_sets,
                affected_rows,
                error_message_
            ))
        {
            const std::string original_error = error_message_;

            std::string rollback_error;

            database_.rollback(rollback_error);

            FAIL()
                << "第 " << batch_index
                << " 批插入失败: " << original_error
                << "；回滚信息: " << rollback_error;
        }


        if (affected_rows != static_cast<int>(kBatchSize))
        {
            std::string rollback_error;

            database_.rollback(rollback_error);

            FAIL()
                << "第 " << batch_index
                << " 批影响行数不正确，实际: "
                << affected_rows;
        }


        if (!database_.commit(error_message_))
        {
            const std::string original_error = error_message_;

            std::string rollback_error;

            database_.rollback(rollback_error);

            FAIL()
                << "第 " << batch_index
                << " 批提交失败: " << original_error
                << "；回滚信息: " << rollback_error;
        }


        const auto database_end = Clock::now();

        database_operation_ms +=
            std::chrono::duration<double, std::milli>(
                database_end - database_start
            ).count();

        total_inserted += affected_rows;
    }


    const auto insert_end = Clock::now();

    const double insert_wall_seconds =
        std::chrono::duration<double>(
            insert_end - insert_start
        ).count();

    const double database_seconds =
        database_operation_ms / 1000.0;


    // ==================================================
    // 4. 验证插入结果
    // ==================================================

    EXPECT_EQ(total_inserted, kTotalRecords);

    std::int64_t actual_count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            database_,
            "SELECT COUNT(*) FROM messages;",
            {},
            actual_count,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(actual_count, kTotalRecords);


    // ==================================================
    // 5. 完整查询 100 万条数据
    // ==================================================

    SQLiteRows rows;

    const auto query_start = Clock::now();

    ASSERT_TRUE(
        database_.query(
            "SELECT id, timestamp, message FROM messages ORDER BY id;",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    const auto query_end = Clock::now();

    const double query_seconds =
        std::chrono::duration<double>(
            query_end - query_start
        ).count();


    // ==================================================
    // 6. 验证查询结果
    // ==================================================

    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kTotalRecords));

    ASSERT_EQ(rows.front().size(), 3U);

    ASSERT_EQ(rows.back().size(), 3U);


    const auto* first_id =
        std::get_if<std::int64_t>(&rows.front()[0]);

    const auto* last_id =
        std::get_if<std::int64_t>(&rows.back()[0]);

    ASSERT_NE(first_id, nullptr);

    ASSERT_NE(last_id, nullptr);

    EXPECT_EQ(*first_id, 1);

    EXPECT_EQ(*last_id, kTotalRecords);


    // ==================================================
    // 7. 测试时间索引查询
    // ==================================================

    constexpr int kIndexedQueryCount = 1000;

    const std::string indexed_sql =
        "SELECT id FROM messages WHERE timestamp = ?;";

    SQLiteRows indexed_rows;

    const auto indexed_start = Clock::now();

    for (int i = 0; i < kIndexedQueryCount; ++i)
    {
        const std::int64_t sequence =
            (static_cast<std::int64_t>(i) * 9973)
            % kTotalRecords + 1;

        indexed_rows.clear();

        ASSERT_TRUE(
            database_.query(
                indexed_sql,
                {
                    kBaseTimestamp + sequence
                },
                indexed_rows,
                error_message_
            )
        ) << error_message_;

        ASSERT_EQ(indexed_rows.size(), 1U);
    }

    const auto indexed_end = Clock::now();

    const double indexed_query_ms =
        std::chrono::duration<double, std::milli>(
            indexed_end - indexed_start
        ).count();


    // ==================================================
    // 8. 输出性能报告
    // ==================================================

    const double insert_wall_throughput =
        static_cast<double>(kTotalRecords) /
        insert_wall_seconds;

    const double database_throughput =
        static_cast<double>(kTotalRecords) /
        database_seconds;

    const double query_throughput =
        static_cast<double>(kTotalRecords) /
        query_seconds;

    const double average_batch_ms =
        database_operation_ms /
        static_cast<double>(kBatchCount);

    const double average_indexed_query_ms =
        indexed_query_ms /
        static_cast<double>(kIndexedQueryCount);


    std::cout
        << "\n"
        << "==================================================\n"
        << "          SQLite Performance Report\n"
        << "==================================================\n"

        << std::fixed
        << std::setprecision(2)

        << "\n[Insert Performance]\n"

        << "Total records: "
        << kTotalRecords
        << "\n"

        << "Batch size: "
        << kBatchSize
        << "\n"

        << "Batch count: "
        << kBatchCount
        << "\n"

        << "Transaction count: "
        << kBatchCount
        << "\n"

        << "Total insert wall time: "
        << insert_wall_seconds
        << " seconds\n"

        << "Database operation time: "
        << database_seconds
        << " seconds\n"

        << "Average database batch time: "
        << average_batch_ms
        << " ms\n"

        << "End-to-end insert throughput: "
        << insert_wall_throughput
        << " records/s\n"

        << "Database operation throughput: "
        << database_throughput
        << " records/s\n"


        << "\n[Full Query Performance]\n"

        << "Queried records: "
        << rows.size()
        << "\n"

        << "Total query time: "
        << query_seconds
        << " seconds\n"

        << "Query throughput: "
        << query_throughput
        << " records/s\n"


        << "\n[Indexed Query Performance]\n"

        << "Query count: "
        << kIndexedQueryCount
        << "\n"

        << "Total indexed query time: "
        << indexed_query_ms
        << " ms\n"

        << "Average indexed query time: "
        << average_indexed_query_ms
        << " ms\n"


        << "\n==================================================\n"
        << std::endl;


    // 释放完整查询结果，避免测试清理时保留大量内存
    rows.clear();
    rows.shrink_to_fit();
}

