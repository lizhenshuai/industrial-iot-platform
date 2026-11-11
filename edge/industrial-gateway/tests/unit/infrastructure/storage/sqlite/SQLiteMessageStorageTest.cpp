
#include "SQLiteMessageStorage.hpp"
#include "SQLiteDatabase.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>


namespace fs = std::filesystem;

namespace sqlite_storage =
    industrial_gateway::infrastructure::storage::sqlite;

namespace ports =
    industrial_gateway::application::ports;

using industrial_gateway::config::StorageConfig;

using sqlite_storage::SQLiteDatabase;
using sqlite_storage::SQLiteMessageStorage;
using sqlite_storage::SQLiteRows;

using ports::MessageToStore;
using ports::StoredMessage;


// ==================================================
// SQLite 消息存储测试夹具
// ==================================================

class SQLiteMessageStorageTest : public ::testing::Test
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

        const auto unique_id =
            sequence.fetch_add(1, std::memory_order_relaxed);


        // 每个测试用例使用独立目录
        test_directory_ =
            fs::temp_directory_path() /
            "industrial_gateway_message_storage_tests" /
            (
                "test_" +
                std::to_string(timestamp) +
                "_" +
                std::to_string(unique_id)
            );


        std::error_code ec;

        fs::create_directories(
            test_directory_,
            ec
        );

        ASSERT_FALSE(ec)
            << "创建测试目录失败: " << ec.message();


        // 故意设置多级不存在的父目录，
        // 验证消息存储是否能够自动创建目录。
        database_path_ =
            test_directory_ /
            "data" /
            "gateway.db";


        config_.database_path =
            database_path_.string();

        config_.busy_timeout_ms = 5000;


        storage_ =
            std::make_unique<SQLiteMessageStorage>(
                config_
            );
    }


    // ==================================================
    // 测试清理
    // ==================================================

    void TearDown() override
    {
        // 先销毁消息存储，释放数据库连接
        storage_.reset();

        // 关闭辅助数据库连接
        inspection_database_.close();


        // 只清理当前测试用例创建的独立目录
        if (!test_directory_.empty())
        {
            std::error_code ec;

            const fs::file_status status =
                fs::symlink_status(
                    test_directory_,
                    ec
                );

            if (ec == std::errc::no_such_file_or_directory)
            {
                return;
            }

            ASSERT_FALSE(ec)
                << "检查测试目录失败: " << ec.message();

            ASSERT_TRUE(fs::is_directory(status))
                << "测试目录不是普通目录";

            ASSERT_FALSE(fs::is_symlink(status))
                << "拒绝清理符号链接路径";

            fs::remove_all(
                test_directory_,
                ec
            );

            EXPECT_FALSE(ec)
                << "清理测试目录失败: " << ec.message();
        }
    }


    // ==================================================
    // 初始化消息存储
    // ==================================================

    bool initializeStorage()
    {
        error_message_.clear();

        return storage_->initialize(
            error_message_
        );
    }


    // ==================================================
    // 打开辅助数据库连接
    // ==================================================

    bool openInspectionDatabase()
    {
        if (inspection_database_.isOpen())
        {
            return true;
        }

        error_message_.clear();

        return inspection_database_.open(
            config_.database_path,
            config_.busy_timeout_ms,
            error_message_
        );
    }


    // ==================================================
    // 查询单个整数
    // ==================================================

    bool querySingleInt64(
        SQLiteDatabase& database,
        const std::string& sql,
        std::int64_t& value
    )
    {
        SQLiteRows rows;

        if (
            !database.query(
                sql,
                {},
                rows,
                error_message_
            )
        )
        {
            return false;
        }

        if (
            rows.size() != 1 ||
            rows.front().size() != 1
        )
        {
            error_message_ =
                "查询结果行数或列数异常";

            return false;
        }

        const auto* result =
            std::get_if<std::int64_t>(
                &rows.front().front()
            );

        if (result == nullptr)
        {
            error_message_ =
                "查询结果不是 INTEGER 类型";

            return false;
        }

        value = *result;

        return true;
    }


    // ==================================================
    // 创建消息对象
    // ==================================================

    static MessageToStore makeMessage(
        std::int64_t timestamp,
        std::string message
    )
    {
        MessageToStore result{};

        result.timestamp_ms = timestamp;

        result.message = std::move(message);

        return result;
    }


protected:

    StorageConfig config_;

    std::unique_ptr<SQLiteMessageStorage> storage_;

    // 独立连接，用于检查数据库内部数据和制造测试故障
    SQLiteDatabase inspection_database_;

    fs::path test_directory_;

    fs::path database_path_;

    std::string error_message_;
};


// ==================================================
// 第一组：初始化测试
// ==================================================


// 正常初始化、自动创建目录、创建表和索引
TEST_F(
    SQLiteMessageStorageTest,
    InitializeCreatesDatabaseDirectoryAndSchema
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    EXPECT_TRUE(
        fs::is_directory(database_path_.parent_path())
    );

    EXPECT_TRUE(
        fs::is_regular_file(database_path_)
    );


    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    // --------------------------------------------------
    // 1. 检查 messages 表字段
    // --------------------------------------------------

    SQLiteRows rows;

    ASSERT_TRUE(
        inspection_database_.query(
            "PRAGMA table_info(messages);",
            {},
            rows,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(rows.size(), 3U);

    std::vector<std::string> column_names;

    for (const auto& row : rows)
    {
        ASSERT_EQ(row.size(), 6U);

        const auto* name =
            std::get_if<std::string>(&row[1]);

        ASSERT_NE(name, nullptr);

        column_names.push_back(*name);
    }

    EXPECT_EQ(
        column_names,
        (
            std::vector<std::string>{
                "id",
                "timestamp",
                "message"
            }
        )
    );


    // --------------------------------------------------
    // 2. 检查时间戳索引
    // --------------------------------------------------

    std::int64_t index_count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            inspection_database_,
            R"(
                SELECT COUNT(*)
                FROM sqlite_master
                WHERE type = 'index'
                AND name = 'idx_timestamp';
            )",
            index_count
        )
    ) << error_message_;

    EXPECT_EQ(index_count, 1);


    // --------------------------------------------------
    // 3. 检查索引字段
    // --------------------------------------------------

    rows.clear();

    ASSERT_TRUE(
        inspection_database_.query(
            "PRAGMA index_info(idx_timestamp);",
            {},
            rows,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(rows.size(), 1U);

    ASSERT_EQ(rows[0].size(), 3U);

    ASSERT_TRUE(
        std::holds_alternative<std::string>(rows[0][2])
    );

    EXPECT_EQ(
        std::get<std::string>(rows[0][2]),
        "timestamp"
    );
}


// 重复初始化应该成功，不应该重复创建或影响数据
TEST_F(
    SQLiteMessageStorageTest,
    InitializeIsIdempotent
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001")
            },
            error_message_
        )
    ) << error_message_;


    error_message_ = "old error";


    EXPECT_TRUE(
        storage_->initialize(error_message_)
    );

    EXPECT_TRUE(error_message_.empty());


    std::vector<StoredMessage> messages;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            messages,
            error_message_
        )
    ) << error_message_;

    ASSERT_EQ(messages.size(), 1U);

    EXPECT_EQ(
        messages[0].message,
        "message_001"
    );
}


// ==================================================
// 第二组：初始化状态和参数测试
// ==================================================


// 未初始化时，不允许执行非空批量操作
TEST_F(
    SQLiteMessageStorageTest,
    RejectOperationsBeforeInitialization
)
{
    EXPECT_FALSE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001")
            },
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // 预先放入数据，验证失败时输出结果会被清空
    std::vector<StoredMessage> messages(1);

    messages[0].id = 999;


    EXPECT_FALSE(
        storage_->queryBatch(
            10,
            messages,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_TRUE(messages.empty());


    EXPECT_FALSE(
        storage_->deleteBatch(
            {1},
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());
}


// 空批次是成功的空操作
TEST_F(
    SQLiteMessageStorageTest,
    EmptyBatchesAreSuccessfulNoOpsBeforeInitialization
)
{
    error_message_ = "old error";

    EXPECT_TRUE(
        storage_->insertBatch(
            {},
            error_message_
        )
    );

    EXPECT_TRUE(error_message_.empty());


    error_message_ = "old error";

    EXPECT_TRUE(
        storage_->deleteBatch(
            {},
            error_message_
        )
    );

    EXPECT_TRUE(error_message_.empty());
}


// LIMIT 为 0 应该失败，并清空输出结果
TEST_F(
    SQLiteMessageStorageTest,
    RejectZeroQueryLimit
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    std::vector<StoredMessage> messages(1);

    messages[0].id = 999;


    EXPECT_FALSE(
        storage_->queryBatch(
            0,
            messages,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_TRUE(messages.empty());
}


// LIMIT 超出 int64_t 范围时应该失败
TEST_F(
    SQLiteMessageStorageTest,
    RejectQueryLimitExceedingInt64Range
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    const auto max_limit =
        std::numeric_limits<std::size_t>::max();


    if (
        static_cast<std::uintmax_t>(max_limit) <=
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::int64_t>::max()
        )
    )
    {
        GTEST_SKIP()
            << "当前平台 size_t 最大值没有超过 int64_t";
    }


    std::vector<StoredMessage> messages(1);

    EXPECT_FALSE(
        storage_->queryBatch(
            max_limit,
            messages,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());

    EXPECT_TRUE(messages.empty());
}


// ==================================================
// 第三组：批量插入测试
// ==================================================


// 插入多条消息，并验证字段和插入顺序
TEST_F(
    SQLiteMessageStorageTest,
    InsertBatchAndQueryPreserveDataAndOrder
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    const std::vector<MessageToStore> inputs = {

        makeMessage(
            3000,
            R"({"device":"motor-001","temperature":36.5})"
        ),

        makeMessage(
            1000,
            R"({"设备":"电机","状态":"运行"})"
        ),

        makeMessage(
            2000,
            ""
        )
    };


    ASSERT_TRUE(
        storage_->insertBatch(
            inputs,
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> outputs;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            outputs,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(outputs.size(), inputs.size());


    for (
        std::size_t i = 0;
        i < inputs.size();
        ++i
    )
    {
        EXPECT_GT(outputs[i].id, 0);

        EXPECT_EQ(
            outputs[i].timestamp_ms,
            inputs[i].timestamp_ms
        );

        EXPECT_EQ(
            outputs[i].message,
            inputs[i].message
        );


        // ID 必须按照插入顺序递增
        if (i > 0)
        {
            EXPECT_GT(
                outputs[i].id,
                outputs[i - 1].id
            );
        }
    }
}


// 批量插入 100 条数据
TEST_F(
    SQLiteMessageStorageTest,
    InsertBatch100Messages
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    constexpr std::size_t kBatchSize = 100;

    std::vector<MessageToStore> inputs;

    inputs.reserve(kBatchSize);


    for (
        std::size_t i = 0;
        i < kBatchSize;
        ++i
    )
    {
        inputs.push_back(
            makeMessage(
                static_cast<std::int64_t>(1000 + i),
                "message_" + std::to_string(i)
            )
        );
    }


    ASSERT_TRUE(
        storage_->insertBatch(
            inputs,
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> outputs;

    ASSERT_TRUE(
        storage_->queryBatch(
            kBatchSize,
            outputs,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(outputs.size(), kBatchSize);


    for (
        std::size_t i = 0;
        i < kBatchSize;
        ++i
    )
    {
        EXPECT_EQ(
            outputs[i].timestamp_ms,
            inputs[i].timestamp_ms
        );

        EXPECT_EQ(
            outputs[i].message,
            inputs[i].message
        );
    }
}


// ==================================================
// 第四组：批量查询测试
// ==================================================


// 查询数量限制和 FIFO 顺序
TEST_F(
    SQLiteMessageStorageTest,
    QueryBatchRespectsLimitAndFIFOOrder
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001"),
                makeMessage(2000, "message_002"),
                makeMessage(3000, "message_003"),
                makeMessage(4000, "message_004"),
                makeMessage(5000, "message_005")
            },
            error_message_
        )
    ) << error_message_;


    // 只查询前两条
    std::vector<StoredMessage> first_batch;

    ASSERT_TRUE(
        storage_->queryBatch(
            2,
            first_batch,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(first_batch.size(), 2U);

    EXPECT_EQ(
        first_batch[0].message,
        "message_001"
    );

    EXPECT_EQ(
        first_batch[1].message,
        "message_002"
    );


    // 查询全部五条
    std::vector<StoredMessage> all_messages;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            all_messages,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(all_messages.size(), 5U);


    // 再次查询不会自动删除已经查询的数据
    EXPECT_EQ(
        all_messages[0].id,
        first_batch[0].id
    );

    EXPECT_EQ(
        all_messages[1].id,
        first_batch[1].id
    );
}


// 没有待发送数据时，查询应该成功并返回空集合
TEST_F(
    SQLiteMessageStorageTest,
    QueryBatchReturnsEmptyWhenNoMessagesExist
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    std::vector<StoredMessage> messages;

    ASSERT_TRUE(
        storage_->queryBatch(
            100,
            messages,
            error_message_
        )
    ) << error_message_;


    EXPECT_TRUE(messages.empty());
}


// ==================================================
// 第五组：批量删除测试
// ==================================================


// 删除指定 ID，不存在的 ID 和重复 ID 不应该导致失败
TEST_F(
    SQLiteMessageStorageTest,
    DeleteBatchRemovesOnlySpecifiedMessages
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001"),
                makeMessage(2000, "message_002"),
                makeMessage(3000, "message_003"),
                makeMessage(4000, "message_004")
            },
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> before_delete;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            before_delete,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(before_delete.size(), 4U);


    // 删除第二条和第四条
    // 同时包含重复 ID 和不存在的 ID
    const std::vector<std::int64_t> ids = {

        before_delete[1].id,

        before_delete[3].id,

        before_delete[1].id,

        -1
    };


    ASSERT_TRUE(
        storage_->deleteBatch(
            ids,
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> after_delete;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            after_delete,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(after_delete.size(), 2U);


    EXPECT_EQ(
        after_delete[0].id,
        before_delete[0].id
    );

    EXPECT_EQ(
        after_delete[1].id,
        before_delete[2].id
    );
}


// 删除全部消息
TEST_F(
    SQLiteMessageStorageTest,
    DeleteBatchRemovesAllSpecifiedMessages
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001"),
                makeMessage(2000, "message_002"),
                makeMessage(3000, "message_003")
            },
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> before_delete;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            before_delete,
            error_message_
        )
    ) << error_message_;


    std::vector<std::int64_t> ids;

    for (const auto& message : before_delete)
    {
        ids.push_back(message.id);
    }


    ASSERT_TRUE(
        storage_->deleteBatch(
            ids,
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> after_delete;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            after_delete,
            error_message_
        )
    ) << error_message_;


    EXPECT_TRUE(after_delete.empty());
}


// ==================================================
// 第六组：事务原子性测试
// ==================================================


// 插入中途失败，验证整个批次回滚
TEST_F(
    SQLiteMessageStorageTest,
    InsertBatchRollsBackEntireTransactionOnFailure
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    // --------------------------------------------------
    // 1. 创建故障注入 Trigger
    // --------------------------------------------------

    ASSERT_TRUE(
        inspection_database_.executeRaw(
            R"(
                CREATE TRIGGER reject_message_insert
                BEFORE INSERT ON messages
                WHEN NEW.message = 'reject'
                BEGIN
                    SELECT RAISE(
                        ABORT,
                        'forced insert failure'
                    );
                END;
            )",
            error_message_
        )
    ) << error_message_;


    inspection_database_.close();


    // --------------------------------------------------
    // 2. 插入三条消息，中间一条故意失败
    // --------------------------------------------------

    EXPECT_FALSE(
        storage_->insertBatch(
            {
                makeMessage(1000, "valid_before"),

                makeMessage(2000, "reject"),

                makeMessage(3000, "valid_after")
            },
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // --------------------------------------------------
    // 3. 验证整个事务回滚
    // --------------------------------------------------

    std::vector<StoredMessage> messages;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            messages,
            error_message_
        )
    ) << error_message_;


    EXPECT_TRUE(messages.empty());


    // --------------------------------------------------
    // 4. 删除故障 Trigger
    // --------------------------------------------------

    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;

    ASSERT_TRUE(
        inspection_database_.executeRaw(
            "DROP TRIGGER reject_message_insert;",
            error_message_
        )
    ) << error_message_;

    inspection_database_.close();


    // --------------------------------------------------
    // 5. 验证回滚后仍然可以正常插入
    // --------------------------------------------------

    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(4000, "valid_after_rollback")
            },
            error_message_
        )
    ) << error_message_;


    messages.clear();

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            messages,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(messages.size(), 1U);

    EXPECT_EQ(
        messages[0].message,
        "valid_after_rollback"
    );
}


// 删除中途失败，验证整个批次回滚
TEST_F(
    SQLiteMessageStorageTest,
    DeleteBatchRollsBackEntireTransactionOnFailure
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "message_001"),
                makeMessage(2000, "message_002"),
                makeMessage(3000, "message_003")
            },
            error_message_
        )
    ) << error_message_;


    std::vector<StoredMessage> before_delete;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            before_delete,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(before_delete.size(), 3U);


    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    // --------------------------------------------------
    // 1. 创建故障 Trigger
    // --------------------------------------------------

    const std::string trigger_sql =
        "CREATE TRIGGER reject_message_delete "
        "BEFORE DELETE ON messages "
        "WHEN OLD.id = " +
        std::to_string(before_delete[1].id) +
        " BEGIN "
        "SELECT RAISE(ABORT, 'forced delete failure'); "
        "END;";


    ASSERT_TRUE(
        inspection_database_.executeRaw(
            trigger_sql,
            error_message_
        )
    ) << error_message_;


    inspection_database_.close();


    // --------------------------------------------------
    // 2. 批量删除，第二条故意失败
    // --------------------------------------------------

    EXPECT_FALSE(
        storage_->deleteBatch(
            {
                before_delete[0].id,
                before_delete[1].id,
                before_delete[2].id
            },
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // --------------------------------------------------
    // 3. 验证第一条删除操作也被回滚
    // --------------------------------------------------

    std::vector<StoredMessage> after_failure;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            after_failure,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(after_failure.size(), 3U);


    for (
        std::size_t i = 0;
        i < before_delete.size();
        ++i
    )
    {
        EXPECT_EQ(
            after_failure[i].id,
            before_delete[i].id
        );
    }


    // --------------------------------------------------
    // 4. 删除故障 Trigger
    // --------------------------------------------------

    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;

    ASSERT_TRUE(
        inspection_database_.executeRaw(
            "DROP TRIGGER reject_message_delete;",
            error_message_
        )
    ) << error_message_;

    inspection_database_.close();


    // --------------------------------------------------
    // 5. 再次删除，应该成功
    // --------------------------------------------------

    ASSERT_TRUE(
        storage_->deleteBatch(
            {
                before_delete[0].id,
                before_delete[1].id,
                before_delete[2].id
            },
            error_message_
        )
    ) << error_message_;


    after_failure.clear();

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            after_failure,
            error_message_
        )
    ) << error_message_;


    EXPECT_TRUE(after_failure.empty());
}


// ==================================================
// 第七组：数据持久化测试
// ==================================================


// 销毁消息存储对象后，重新初始化仍然能够读取数据
TEST_F(
    SQLiteMessageStorageTest,
    DataPersistsAfterStorageRecreation
)
{
    ASSERT_TRUE(initializeStorage())
        << error_message_;


    ASSERT_TRUE(
        storage_->insertBatch(
            {
                makeMessage(1000, "persistent_message_001"),
                makeMessage(2000, "persistent_message_002")
            },
            error_message_
        )
    ) << error_message_;


    // --------------------------------------------------
    // 1. 销毁原消息存储对象
    // --------------------------------------------------

    storage_.reset();


    // --------------------------------------------------
    // 2. 使用同一个数据库路径创建新对象
    // --------------------------------------------------

    storage_ =
        std::make_unique<SQLiteMessageStorage>(
            config_
        );


    ASSERT_TRUE(initializeStorage())
        << error_message_;


    // --------------------------------------------------
    // 3. 查询之前的数据
    // --------------------------------------------------

    std::vector<StoredMessage> messages;

    ASSERT_TRUE(
        storage_->queryBatch(
            10,
            messages,
            error_message_
        )
    ) << error_message_;


    ASSERT_EQ(messages.size(), 2U);


    EXPECT_EQ(
        messages[0].message,
        "persistent_message_001"
    );

    EXPECT_EQ(
        messages[1].message,
        "persistent_message_002"
    );
}


// ==================================================
// 第八组：初始化失败恢复测试
// ==================================================


// 初始化遇到错误表结构后，修复数据库并重新初始化
TEST_F(
    SQLiteMessageStorageTest,
    InitializeCanRetryAfterSchemaFailure
)
{
    // --------------------------------------------------
    // 1. 手动创建一个错误的 messages 表
    // --------------------------------------------------

    std::error_code ec;

    fs::create_directories(
        database_path_.parent_path(),
        ec
    );

    ASSERT_FALSE(ec)
        << ec.message();


    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    ASSERT_TRUE(
        inspection_database_.executeRaw(
            R"(
                CREATE TABLE messages (
                    id INTEGER PRIMARY KEY,
                    wrong_column TEXT
                );
            )",
            error_message_
        )
    ) << error_message_;


    inspection_database_.close();


    // --------------------------------------------------
    // 2. 初始化应该失败
    // --------------------------------------------------

    EXPECT_FALSE(initializeStorage());

    EXPECT_FALSE(error_message_.empty());


    // --------------------------------------------------
    // 3. 修复错误表结构
    // --------------------------------------------------

    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    ASSERT_TRUE(
        inspection_database_.executeRaw(
            "DROP TABLE messages;",
            error_message_
        )
    ) << error_message_;


    inspection_database_.close();


    // --------------------------------------------------
    // 4. 再次初始化应该成功
    // --------------------------------------------------

    ASSERT_TRUE(initializeStorage())
        << error_message_;


    // --------------------------------------------------
    // 5. 验证新表结构正常
    // --------------------------------------------------

    ASSERT_TRUE(openInspectionDatabase())
        << error_message_;


    std::int64_t table_count = 0;

    ASSERT_TRUE(
        querySingleInt64(
            inspection_database_,
            R"(
                SELECT COUNT(*)
                FROM sqlite_master
                WHERE type = 'table'
                AND name = 'messages';
            )",
            table_count
        )
    ) << error_message_;


    EXPECT_EQ(table_count, 1);
}
