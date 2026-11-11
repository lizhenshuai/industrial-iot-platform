
#include "StorageWorker.hpp"
#include "BoundedDataQueue.hpp"

#include "industrial_gateway/application/ports/IMessageStorage.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/domain/enums/CollectionStatus.hpp"
#include "industrial_gateway/domain/model/TelemetryPacket.hpp"
#include "industrial_gateway/domain/types/TelemetryValue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace
{

using namespace industrial_gateway;

using application::ingestion::StorageWorker;

using application::ports::IMessageStorage;
using application::ports::MessageToStore;
using application::ports::StoredMessage;

using common::ILogger;
using common::LogLevel;

using domain::enums::CollectionStatus;
using domain::model::TelemetryPacket;
using domain::types::TelemetryValue;

using infrastructure::concurrency::BoundedDataQueue;


// ==================================================
// 空日志实现
// ==================================================
//
// StorageWorker 依赖 ILogger。
// 测试时不需要真正输出日志。
//
// ==================================================

class NullLogger final : public ILogger
{
public:

    void log(
        LogLevel level,
        std::string_view component,
        std::string_view message
    ) override
    {
        (void)level;
        (void)component;
        (void)message;
    }
};


// ==================================================
// Fake SQLite 消息存储
// ==================================================
//
// 不使用真实 SQLite。
//
// 用于：
// 1. 记录初始化调用次数。
// 2. 记录批量写入调用次数。
// 3. 模拟初始化失败。
// 4. 模拟批量写入失败。
// 5. 保存成功写入的消息。
// 6. 保存每次写入尝试的消息。
//
// ==================================================

class FakeMessageStorage final : public IMessageStorage
{
public:

    // 模拟初始化失败次数
    std::size_t fail_initialize_attempts{0};

    // 模拟批量写入失败次数
    std::size_t fail_insert_attempts{0};


    // ==================================================
    // 初始化
    // ==================================================

    bool initialize(
        std::string& error_message
    ) override
    {
        const std::size_t attempt =
            initialize_calls_.fetch_add(1) + 1;

        if (attempt <= fail_initialize_attempts)
        {
            error_message = "模拟 SQLite 初始化失败";

            return false;
        }

        error_message.clear();

        return true;
    }


    // ==================================================
    // 批量插入
    // ==================================================

    bool insertBatch(
        const std::vector<MessageToStore>& messages,
        std::string& error_message
    ) override
    {
        const std::size_t attempt =
            insert_calls_.fetch_add(1) + 1;

        std::lock_guard<std::mutex> lock(mutex_);

        // 记录每一次写入尝试
        attempted_batches_.push_back(messages);


        // 模拟写入失败
        if (attempt <= fail_insert_attempts)
        {
            error_message = "模拟 SQLite 批量写入失败";

            return false;
        }


        // 写入成功
        successful_batches_.push_back(messages);

        error_message.clear();

        return true;
    }


    // ==================================================
    // 批量查询
    // ==================================================

    bool queryBatch(
        std::size_t limit,
        std::vector<StoredMessage>& messages,
        std::string& error_message
    ) override
    {
        (void)limit;

        messages.clear();

        error_message.clear();

        return true;
    }


    // ==================================================
    // 批量删除
    // ==================================================

    bool deleteBatch(
        const std::vector<std::int64_t>& ids,
        std::string& error_message
    ) override
    {
        (void)ids;

        error_message.clear();

        return true;
    }


    // ==================================================
    // 获取初始化调用次数
    // ==================================================

    std::size_t initializeCalls() const
    {
        return initialize_calls_.load();
    }


    // ==================================================
    // 获取插入调用次数
    // ==================================================

    std::size_t insertCalls() const
    {
        return insert_calls_.load();
    }


    // ==================================================
    // 获取成功写入的批次
    // ==================================================

    std::vector<std::vector<MessageToStore>>
    successfulBatches() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        return successful_batches_;
    }


    // ==================================================
    // 获取所有写入尝试
    // ==================================================

    std::vector<std::vector<MessageToStore>>
    attemptedBatches() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        return attempted_batches_;
    }


private:

    std::atomic<std::size_t> initialize_calls_{0};

    std::atomic<std::size_t> insert_calls_{0};

    mutable std::mutex mutex_;

    std::vector<std::vector<MessageToStore>>
        attempted_batches_;

    std::vector<std::vector<MessageToStore>>
        successful_batches_;
};


// ==================================================
// 测试辅助函数
// ==================================================


// --------------------------------------------------
// 创建成功采集记录
// --------------------------------------------------

TelemetryPacket makeSuccessRecord(
    std::string message_id,
    TelemetryValue value,
    std::int64_t timestamp_ms = 1000
)
{
    TelemetryPacket record;

    record.message_id = std::move(message_id);

    record.timestamp_ms = timestamp_ms;
    record.devices.push_back({10001, CollectionStatus::Success,
                              {{"temperature", std::move(value)}}});

    return record;
}


// --------------------------------------------------
// 创建失败采集记录
// --------------------------------------------------

TelemetryPacket makeFailureRecord(
    std::string message_id
)
{
    TelemetryPacket record;

    record.message_id = std::move(message_id);

    record.timestamp_ms = 1000;
    record.devices.push_back({10001, CollectionStatus::Failure, {}});

    return record;
}


// --------------------------------------------------
// 展开存储批次
// --------------------------------------------------

std::vector<MessageToStore> flattenBatches(
    const std::vector<std::vector<MessageToStore>>& batches
)
{
    std::vector<MessageToStore> messages;

    for (const auto& batch : batches)
    {
        messages.insert(
            messages.end(),
            batch.begin(),
            batch.end()
        );
    }

    return messages;
}


// ==================================================
// 一、批量写入与队列排空测试
// ==================================================


// --------------------------------------------------
// 1. 批量写入按照100条进行处理
// --------------------------------------------------

TEST(
    StorageWorkerTest,
    WritesRecordsInBatchesOf100
)
{
    BoundedDataQueue queue(256);

    FakeMessageStorage storage;

    NullLogger logger;


    constexpr std::size_t TOTAL_RECORDS = 205;


    // --------------------------------------------------
    // 1. 向队列写入205条记录
    // --------------------------------------------------

    for (
        std::size_t i = 0;
        i < TOTAL_RECORDS;
        ++i
    )
    {
        auto record = makeSuccessRecord(
            "message-" + std::to_string(i),
            TelemetryValue{
                static_cast<double>(i)
            },
            1000 + static_cast<std::int64_t>(i)
        );

        ASSERT_TRUE(
            queue.push(std::move(record))
        );
    }


    // --------------------------------------------------
    // 2. 关闭队列
    // --------------------------------------------------

    queue.close();


    // --------------------------------------------------
    // 3. 启动 StorageWorker
    // --------------------------------------------------

    StorageWorker worker(
        queue,
        storage,
        logger
    );

    worker.run();


    // --------------------------------------------------
    // 4. 检查批次数量
    // --------------------------------------------------

    const auto batches =
        storage.successfulBatches();

    ASSERT_EQ(batches.size(), 3U);


    // --------------------------------------------------
    // 5. 检查批次大小
    // --------------------------------------------------

    EXPECT_EQ(batches[0].size(), 100U);

    EXPECT_EQ(batches[1].size(), 100U);

    EXPECT_EQ(batches[2].size(), 5U);


    // --------------------------------------------------
    // 6. 检查总数据量
    // --------------------------------------------------

    const auto messages =
        flattenBatches(batches);

    ASSERT_EQ(messages.size(), TOTAL_RECORDS);


    // --------------------------------------------------
    // 7. 检查数据顺序
    // --------------------------------------------------

    for (
        std::size_t i = 0;
        i < TOTAL_RECORDS;
        ++i
    )
    {
        EXPECT_EQ(
            messages[i].timestamp_ms,
            1000 + static_cast<std::int64_t>(i)
        );

        EXPECT_NE(
            messages[i].message.find(
                "\"message_id\":\"message-" +
                std::to_string(i) +
                "\""
            ),
            std::string::npos
        );
    }


    // 队列应该已经完全排空
    EXPECT_EQ(queue.size(), 0U);
}


// ==================================================
// 二、JSON 序列化测试
// ==================================================


// --------------------------------------------------
// 2. 支持不同 TelemetryValue 类型及 JSON 转义
// --------------------------------------------------

TEST(
    StorageWorkerTest,
    SerializesJsonFieldsTypesAndEscapesStrings
)
{
    BoundedDataQueue queue(16);

    FakeMessageStorage storage;

    NullLogger logger;


    // --------------------------------------------------
    // 1. bool
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "bool",
                TelemetryValue{true}
            )
        )
    );


    // --------------------------------------------------
    // 2. int64_t
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "signed",
                TelemetryValue{
                    std::int64_t{-123}
                }
            )
        )
    );


    // --------------------------------------------------
    // 3. uint64_t
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "unsigned",
                TelemetryValue{
                    std::numeric_limits<
                        std::uint64_t
                    >::max()
                }
            )
        )
    );


    // --------------------------------------------------
    // 4. double
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "double",
                TelemetryValue{36.5}
            )
        )
    );


    // --------------------------------------------------
    // 5. 字符串特殊字符
    // --------------------------------------------------

    const std::string special_string =
        "motor \"A\"\\line\n\t温度";

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "string",
                TelemetryValue{special_string}
            )
        )
    );


    // --------------------------------------------------
    // 6. Failure 状态
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeFailureRecord("failure")
        )
    );


    // --------------------------------------------------
    // 7. 非有限 double
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "infinity",
                TelemetryValue{
                    std::numeric_limits<double>::infinity()
                }
            )
        )
    );


    // --------------------------------------------------
    // 8. 关闭队列并运行
    // --------------------------------------------------

    queue.close();

    StorageWorker worker(
        queue,
        storage,
        logger
    );

    worker.run();


    // --------------------------------------------------
    // 9. 获取 JSON 消息
    // --------------------------------------------------

    const auto messages =
        flattenBatches(
            storage.successfulBatches()
        );

    ASSERT_EQ(messages.size(), 7U);


    // --------------------------------------------------
    // 10. bool
    // --------------------------------------------------

    EXPECT_NE(
        messages[0].message.find("\"value\":true"),
        std::string::npos
    );


    // --------------------------------------------------
    // 11. int64_t
    // --------------------------------------------------

    EXPECT_NE(
        messages[1].message.find("\"value\":-123"),
        std::string::npos
    );


    // --------------------------------------------------
    // 12. uint64_t
    // --------------------------------------------------

    EXPECT_NE(
        messages[2].message.find(
            "\"value\":" +
            std::to_string(
                std::numeric_limits<std::uint64_t>::max()
            )
        ),
        std::string::npos
    );


    // --------------------------------------------------
    // 13. double
    // --------------------------------------------------

    EXPECT_NE(
        messages[3].message.find("\"value\":36.5"),
        std::string::npos
    );


    // --------------------------------------------------
    // 14. 字符串转义
    // --------------------------------------------------

    const std::string expected_json_value =
        "\"value\":\"motor \\\"A\\\"\\\\line\\n\\t温度\"";

    EXPECT_NE(
        messages[4].message.find(expected_json_value),
        std::string::npos
    );


    // --------------------------------------------------
    // 15. Failure 状态
    // --------------------------------------------------

    EXPECT_NE(
        messages[5].message.find("\"records\":[]"),
        std::string::npos
    );

    EXPECT_NE(
        messages[5].message.find(
            "\"status\":\"Failure\""
        ),
        std::string::npos
    );


    // --------------------------------------------------
    // 16. Infinity 异常诊断
    // --------------------------------------------------

    EXPECT_NE(
        messages[6].message.find("\"value\":null"),
        std::string::npos
    );

    EXPECT_NE(
        messages[6].message.find(
            "\"serialization_error\":"
        ),
        std::string::npos
    );

    EXPECT_NE(
        messages[6].message.find(
            "\"value_serialization_error\":"
        ),
        std::string::npos
    );

    EXPECT_NE(
        messages[6].message.find(
            "\"value_raw\":\"Infinity\""
        ),
        std::string::npos
    );
}


// ==================================================
// 三、SQLite 写入失败重试测试
// ==================================================


// --------------------------------------------------
// 3. 写入失败后重新初始化并重试相同批次
// --------------------------------------------------

TEST(
    StorageWorkerTest,
    RetriesFailedInsertWithoutLosingBatch
)
{
    BoundedDataQueue queue(10);

    FakeMessageStorage storage;

    NullLogger logger;


    // 第一次批量写入失败
    storage.fail_insert_attempts = 1;


    // --------------------------------------------------
    // 1. 准备数据
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "retry-message",
                TelemetryValue{36.5}
            )
        )
    );

    queue.close();


    // --------------------------------------------------
    // 2. 运行 StorageWorker
    // --------------------------------------------------

    StorageWorker worker(
        queue,
        storage,
        logger
    );

    worker.run();


    // --------------------------------------------------
    // 3. 检查数据库重新初始化
    // --------------------------------------------------

    EXPECT_EQ(
        storage.initializeCalls(),
        2U
    );


    // --------------------------------------------------
    // 4. 检查插入调用次数
    // --------------------------------------------------

    EXPECT_EQ(
        storage.insertCalls(),
        2U
    );


    // --------------------------------------------------
    // 5. 检查两次尝试的数据完全一致
    // --------------------------------------------------

    const auto attempts =
        storage.attemptedBatches();

    ASSERT_EQ(attempts.size(), 2U);

    ASSERT_EQ(attempts[0].size(), 1U);

    ASSERT_EQ(attempts[1].size(), 1U);

    EXPECT_EQ(
        attempts[0][0].timestamp_ms,
        attempts[1][0].timestamp_ms
    );

    EXPECT_EQ(
        attempts[0][0].message,
        attempts[1][0].message
    );


    // --------------------------------------------------
    // 6. 最终只能成功持久化一次
    // --------------------------------------------------

    const auto successful =
        storage.successfulBatches();

    ASSERT_EQ(successful.size(), 1U);

    ASSERT_EQ(successful[0].size(), 1U);

    EXPECT_EQ(
        successful[0][0].message.find(
            "\"message_id\":\"retry-message\""
        ) != std::string::npos,
        true
    );
}


// ==================================================
// 四、初始化异常恢复测试
// ==================================================


// --------------------------------------------------
// 4. SQLite 初始化失败后恢复
// --------------------------------------------------

TEST(
    StorageWorkerTest,
    RetriesInitializationFailure
)
{
    BoundedDataQueue queue(10);

    FakeMessageStorage storage;

    NullLogger logger;


    // 第一次初始化失败
    storage.fail_initialize_attempts = 1;


    // --------------------------------------------------
    // 1. 准备数据
    // --------------------------------------------------

    ASSERT_TRUE(
        queue.push(
            makeSuccessRecord(
                "init-recovery",
                TelemetryValue{36.5}
            )
        )
    );

    queue.close();


    // --------------------------------------------------
    // 2. 运行 StorageWorker
    // --------------------------------------------------

    StorageWorker worker(
        queue,
        storage,
        logger
    );

    worker.run();


    // --------------------------------------------------
    // 3. 检查初始化重试
    // --------------------------------------------------

    EXPECT_EQ(
        storage.initializeCalls(),
        2U
    );


    // --------------------------------------------------
    // 4. 检查数据最终成功写入
    // --------------------------------------------------

    EXPECT_EQ(
        storage.insertCalls(),
        1U
    );


    const auto successful =
        storage.successfulBatches();

    ASSERT_EQ(successful.size(), 1U);

    ASSERT_EQ(successful[0].size(), 1U);

    EXPECT_NE(
        successful[0][0].message.find(
            "\"message_id\":\"init-recovery\""
        ),
        std::string::npos
    );
}


// ==================================================
// 五、队列关闭排空测试
// ==================================================


// --------------------------------------------------
// 5. 队列关闭后，仍然持久化全部剩余数据
// --------------------------------------------------

TEST(
    StorageWorkerTest,
    DrainsAllRemainingRecordsAfterQueueClose
)
{
    BoundedDataQueue queue(128);

    FakeMessageStorage storage;

    NullLogger logger;


    constexpr std::size_t TOTAL_RECORDS = 75;


    // --------------------------------------------------
    // 1. 填充队列
    // --------------------------------------------------

    for (
        std::size_t i = 0;
        i < TOTAL_RECORDS;
        ++i
    )
    {
        auto record = makeSuccessRecord(
            "drain-" + std::to_string(i),
            TelemetryValue{
                static_cast<std::int64_t>(i)
            },
            2000 + static_cast<std::int64_t>(i)
        );

        ASSERT_TRUE(
            queue.push(std::move(record))
        );
    }


    EXPECT_EQ(
        queue.size(),
        TOTAL_RECORDS
    );


    // --------------------------------------------------
    // 2. 关闭队列
    // --------------------------------------------------

    queue.close();

    EXPECT_TRUE(queue.isClosed());


    // --------------------------------------------------
    // 3. 运行 StorageWorker
    // --------------------------------------------------

    StorageWorker worker(
        queue,
        storage,
        logger
    );

    worker.run();


    // --------------------------------------------------
    // 4. 检查队列是否完全排空
    // --------------------------------------------------

    EXPECT_EQ(queue.size(), 0U);


    // --------------------------------------------------
    // 5. 检查所有数据是否持久化
    // --------------------------------------------------

    const auto messages =
        flattenBatches(
            storage.successfulBatches()
        );

    ASSERT_EQ(
        messages.size(),
        TOTAL_RECORDS
    );


    // --------------------------------------------------
    // 6. 检查 FIFO 顺序
    // --------------------------------------------------

    for (
        std::size_t i = 0;
        i < TOTAL_RECORDS;
        ++i
    )
    {
        EXPECT_EQ(
            messages[i].timestamp_ms,
            2000 + static_cast<std::int64_t>(i)
        );

        EXPECT_NE(
            messages[i].message.find(
                "\"message_id\":\"drain-" +
                std::to_string(i) +
                "\""
            ),
            std::string::npos
        );
    }
}



TEST(StorageWorkerTest, StoresCompleteMultiDevicePacketAndRetriesIdenticalJson)
{
    BoundedDataQueue queue(4);
    FakeMessageStorage storage;
    storage.fail_insert_attempts = 1;
    NullLogger logger;
    TelemetryPacket packet{
        "packet-001", 1794000000000,
        {
            {10001, CollectionStatus::Success,
                {{"temperature", 36.5}, {"speed", std::int64_t{1500}}}},
            {10002, CollectionStatus::Success, {{"current", 12.5}}},
            {10003, CollectionStatus::Failure, {}}
        }
    };
    ASSERT_TRUE(queue.push(std::move(packet)));
    ASSERT_TRUE(queue.push(makeFailureRecord("packet-002")));
    queue.close();
    StorageWorker worker(queue, storage, logger);
    worker.run();
    const auto messages = flattenBatches(storage.successfulBatches());
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].timestamp_ms, 1794000000000);
    EXPECT_EQ(messages[0].message,
        R"({"message_id":"packet-001","timestamp_ms":1794000000000,"devices":[{"device_id":10001,"status":"Success","records":[{"property":"temperature","value":36.5},{"property":"speed","value":1500}]},{"device_id":10002,"status":"Success","records":[{"property":"current","value":12.5}]},{"device_id":10003,"status":"Failure","records":[]}]})");
    const auto attempts = storage.attemptedBatches();
    ASSERT_EQ(attempts.size(), 2U);
    ASSERT_EQ(attempts[0].size(), 2U);
    ASSERT_EQ(attempts[1].size(), 2U);
    EXPECT_EQ(attempts[0][0].message, attempts[1][0].message);
    EXPECT_EQ(attempts[0][1].message, attempts[1][1].message);
}

TEST(StorageWorkerTest, PreservesHealthyDevicesAndNonFiniteDiagnosticsInOnePacket)
{
    BoundedDataQueue queue(1);
    FakeMessageStorage storage;
    NullLogger logger;
    TelemetryPacket packet{"diagnostic", 1234, {
        {10001, CollectionStatus::Success, {
            {"nan", std::numeric_limits<double>::quiet_NaN()},
            {"positive", std::numeric_limits<double>::infinity()},
            {"negative", -std::numeric_limits<double>::infinity()}}},
        {10002, CollectionStatus::Success, {{"current", 12.5}}}
    }};
    ASSERT_TRUE(queue.push(std::move(packet)));
    queue.close();
    StorageWorker worker(queue, storage, logger);
    worker.run();
    const auto messages = flattenBatches(storage.successfulBatches());
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_NE(messages[0].message.find(R"("message_id":"diagnostic")"), std::string::npos);
    EXPECT_NE(messages[0].message.find(R"("value_raw":"NaN")"), std::string::npos);
    EXPECT_NE(messages[0].message.find(R"("value_raw":"Infinity")"), std::string::npos);
    EXPECT_NE(messages[0].message.find(R"("value_raw":"-Infinity")"), std::string::npos);
    EXPECT_NE(messages[0].message.find(
        R"({"device_id":10002,"status":"Success","records":[{"property":"current","value":12.5}]})"),
        std::string::npos);
}

} // namespace
