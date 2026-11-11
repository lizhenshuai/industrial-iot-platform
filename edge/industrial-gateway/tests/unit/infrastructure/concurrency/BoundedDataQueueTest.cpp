
#include "BoundedDataQueue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>


namespace
{

using namespace std::chrono_literals;

using industrial_gateway::application::ports::DataQueuePopStatus;

using industrial_gateway::domain::enums::CollectionStatus;

using industrial_gateway::domain::model::TelemetryPacket;

using industrial_gateway::domain::types::TelemetryValue;

using industrial_gateway::infrastructure::concurrency::BoundedDataQueue;


// ==================================================
// 测试辅助函数
// ==================================================

TelemetryPacket makeRecord(
    std::string messageId,
    std::int64_t deviceId = 10001,
    std::string property = "temperature",
    std::int64_t timestampMs = 1000,
    double value = 36.5
)
{
    TelemetryPacket record;

    record.message_id = std::move(messageId);

    record.timestamp_ms = timestampMs;
    record.devices.push_back({deviceId, CollectionStatus::Success,
                              {{std::move(property), TelemetryValue{value}}}});

    return record;
}


// ==================================================
// 性能测试辅助结构
// ==================================================

struct BenchmarkResult
{
    std::size_t capacity{0};

    bool batch_push{false};

    std::size_t produced{0};

    std::size_t consumed{0};

    double elapsed_ms{0.0};

    double throughput{0.0};

    bool producer_failed{false};
};


// ==================================================
// 队列吞吐量测试
// ==================================================
//
// 4 个生产者 + 1 个消费者。
//
// 比较：
// 1. 单条 push()
// 2. 批量 pushBatch()
//
// 消费者统一使用 waitPopBatch()。
//
// ==================================================

BenchmarkResult runQueueBenchmark(
    std::size_t capacity,
    bool batchPush
)
{
    constexpr std::size_t PRODUCER_COUNT = 4;

    constexpr std::size_t RECORDS_PER_PRODUCER = 10000;

    constexpr std::size_t PRODUCER_BATCH_SIZE = 32;

    constexpr std::size_t CONSUMER_BATCH_SIZE = 256;

    constexpr std::size_t TOTAL_RECORDS =
        PRODUCER_COUNT * RECORDS_PER_PRODUCER;


    BoundedDataQueue queue(capacity);

    std::atomic<bool> start{false};

    std::atomic<bool> producerFailed{false};

    std::atomic<std::size_t> consumed{0};


    // ==================================================
    // 消费者线程
    // ==================================================

    std::thread consumer(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            std::vector<TelemetryPacket> batch;

            while (true)
            {
                const auto status = queue.waitPopBatch(
                    batch,
                    CONSUMER_BATCH_SIZE,
                    1000ms
                );

                if (status == DataQueuePopStatus::DataAvailable)
                {
                    consumed.fetch_add(
                        batch.size(),
                        std::memory_order_relaxed
                    );
                }
                else if (status == DataQueuePopStatus::Closed)
                {
                    break;
                }
            }
        }
    );


    // ==================================================
    // 生产者线程
    // ==================================================

    std::vector<std::thread> producers;

    producers.reserve(PRODUCER_COUNT);

    for (std::size_t producerId = 0;
         producerId < PRODUCER_COUNT;
         ++producerId)
    {
        producers.emplace_back(
            [&, producerId]()
            {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                const std::int64_t deviceId = static_cast<std::int64_t>(producerId);


                // --------------------------------------
                // 批量入队模式
                // --------------------------------------

                if (batchPush)
                {
                    std::size_t sequence = 0;

                    while (sequence < RECORDS_PER_PRODUCER)
                    {
                        const std::size_t count = std::min(
                            PRODUCER_BATCH_SIZE,
                            RECORDS_PER_PRODUCER - sequence
                        );

                        std::vector<TelemetryPacket> records;

                        records.reserve(count);

                        for (std::size_t i = 0; i < count; ++i)
                        {
                            const std::size_t current = sequence + i;

                            records.emplace_back(
                                makeRecord(
                                    std::to_string(deviceId) + "-" + std::to_string(current),
                                    deviceId,
                                    "temperature",
                                    static_cast<std::int64_t>(current)
                                )
                            );
                        }

                        if (!queue.pushBatch(std::move(records)))
                        {
                            producerFailed.store(true);
                            return;
                        }

                        sequence += count;
                    }
                }

                // --------------------------------------
                // 单条入队模式
                // --------------------------------------

                else
                {
                    for (std::size_t i = 0;
                         i < RECORDS_PER_PRODUCER;
                         ++i)
                    {
                        auto record = makeRecord(
                            std::to_string(deviceId) + "-" + std::to_string(i),
                            deviceId,
                            "temperature",
                            static_cast<std::int64_t>(i)
                        );

                        if (!queue.push(std::move(record)))
                        {
                            producerFailed.store(true);
                            return;
                        }
                    }
                }
            }
        );
    }


    // ==================================================
    // 开始性能测试
    // ==================================================

    const auto startTime = std::chrono::steady_clock::now();

    start.store(true, std::memory_order_release);


    // 等待所有生产者完成
    for (auto& producer : producers)
    {
        producer.join();
    }


    // 所有生产者结束后关闭队列
    // 消费者继续排空剩余数据
    queue.close();

    consumer.join();


    const auto endTime = std::chrono::steady_clock::now();

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            endTime - startTime
        ).count();

    const double elapsedSeconds = elapsedMs / 1000.0;

    const std::size_t consumedCount =
        consumed.load(std::memory_order_relaxed);


    BenchmarkResult result;

    result.capacity = capacity;

    result.batch_push = batchPush;

    result.produced = TOTAL_RECORDS;

    result.consumed = consumedCount;

    result.elapsed_ms = elapsedMs;

    result.throughput = elapsedSeconds > 0.0
        ? consumedCount / elapsedSeconds
        : 0.0;

    result.producer_failed =
        producerFailed.load();

    return result;
}


TEST(BoundedDataQueueTest, PreservesCompleteNestedPacket)
{
    BoundedDataQueue queue(1);
    TelemetryPacket packet{"packet-001", 1794000000000, {
        {10001, CollectionStatus::Success,
            {{"temperature", 36.5}, {"speed", std::int64_t{1500}}}},
        {10002, CollectionStatus::Failure, {}}
    }};
    ASSERT_TRUE(queue.push(std::move(packet)));
    EXPECT_EQ(queue.size(), 1U);
    queue.close();
    std::vector<TelemetryPacket> batch;
    ASSERT_EQ(queue.waitPopBatch(batch, 1, 0ms), DataQueuePopStatus::DataAvailable);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].message_id, "packet-001");
    EXPECT_EQ(batch[0].timestamp_ms, 1794000000000);
    ASSERT_EQ(batch[0].devices.size(), 2U);
    EXPECT_EQ(batch[0].devices[0].device_id, 10001);
    ASSERT_EQ(batch[0].devices[0].records.size(), 2U);
    EXPECT_EQ(batch[0].devices[0].records[0].property, "temperature");
    EXPECT_EQ(std::get<double>(batch[0].devices[0].records[0].value), 36.5);
    EXPECT_EQ(std::get<std::int64_t>(batch[0].devices[0].records[1].value), 1500);
    EXPECT_EQ(batch[0].devices[1].device_id, 10002);
    EXPECT_EQ(batch[0].devices[1].status, CollectionStatus::Failure);
    EXPECT_TRUE(batch[0].devices[1].records.empty());
}

} // namespace


// ==================================================
// 一、基础功能测试
// ==================================================


// --------------------------------------------------
// 1. 构造函数与基本状态
// --------------------------------------------------

TEST(BoundedDataQueueTest, ConstructorAndBasicState)
{
    EXPECT_THROW(
        {
            BoundedDataQueue queue(0);
        },
        std::invalid_argument
    );

    BoundedDataQueue queue(100);

    EXPECT_EQ(queue.capacity(), 100U);

    EXPECT_EQ(queue.size(), 0U);

    EXPECT_FALSE(queue.isClosed());
}


// --------------------------------------------------
// 2. 单条入队与出队
// --------------------------------------------------

TEST(BoundedDataQueueTest, PushAndPopSingleRecord)
{
    BoundedDataQueue queue(10);

    auto record = makeRecord("message-001");

    EXPECT_TRUE(queue.push(std::move(record)));

    EXPECT_EQ(queue.size(), 1U);

    std::vector<TelemetryPacket> batch;

    const auto status = queue.waitPopBatch(
        batch,
        1,
        0ms
    );

    EXPECT_EQ(status, DataQueuePopStatus::DataAvailable);

    ASSERT_EQ(batch.size(), 1U);

    EXPECT_EQ(batch[0].message_id, "message-001");

    EXPECT_EQ(queue.size(), 0U);
}


// --------------------------------------------------
// 3. 批量入队与批量出队
// --------------------------------------------------

TEST(BoundedDataQueueTest, PushBatchAndPopBatchPreserveFIFO)
{
    BoundedDataQueue queue(10);

    std::vector<TelemetryPacket> records;

    records.emplace_back(makeRecord("message-001"));

    records.emplace_back(makeRecord("message-002"));

    records.emplace_back(makeRecord("message-003"));


    EXPECT_TRUE(queue.pushBatch(std::move(records)));

    EXPECT_EQ(queue.size(), 3U);


    std::vector<TelemetryPacket> batch;

    auto status = queue.waitPopBatch(
        batch,
        2,
        0ms
    );

    EXPECT_EQ(status, DataQueuePopStatus::DataAvailable);

    ASSERT_EQ(batch.size(), 2U);

    EXPECT_EQ(batch[0].message_id, "message-001");

    EXPECT_EQ(batch[1].message_id, "message-002");


    status = queue.waitPopBatch(
        batch,
        2,
        0ms
    );

    EXPECT_EQ(status, DataQueuePopStatus::DataAvailable);

    ASSERT_EQ(batch.size(), 1U);

    EXPECT_EQ(batch[0].message_id, "message-003");

    EXPECT_EQ(queue.size(), 0U);
}


// --------------------------------------------------
// 4. 空批次入队
// --------------------------------------------------

TEST(BoundedDataQueueTest, EmptyBatchIsSuccessfulNoOp)
{
    BoundedDataQueue queue(10);

    std::vector<TelemetryPacket> records;

    EXPECT_TRUE(queue.pushBatch(std::move(records)));

    EXPECT_EQ(queue.size(), 0U);
}


// --------------------------------------------------
// 5. 批次超过队列容量
// --------------------------------------------------

TEST(BoundedDataQueueTest, OversizedBatchThrowsException)
{
    BoundedDataQueue queue(3);

    std::vector<TelemetryPacket> records;

    for (int i = 0; i < 4; ++i)
    {
        records.emplace_back(
            makeRecord("message-" + std::to_string(i))
        );
    }

    EXPECT_THROW(
        queue.pushBatch(std::move(records)),
        std::invalid_argument
    );

    // 抛出异常前没有移动数据
    EXPECT_EQ(records.size(), 4U);

    EXPECT_EQ(records[0].message_id, "message-0");

    EXPECT_EQ(queue.size(), 0U);
}


// --------------------------------------------------
// 6. waitPopBatch 超时与参数检查
// --------------------------------------------------

TEST(BoundedDataQueueTest, WaitPopBatchTimeoutAndInvalidArguments)
{
    BoundedDataQueue queue(10);

    std::vector<TelemetryPacket> batch;

    batch.emplace_back(makeRecord("stale-message"));

    const auto status = queue.waitPopBatch(
        batch,
        10,
        20ms
    );

    EXPECT_EQ(status, DataQueuePopStatus::Timeout);

    // 每次调用先清空输出容器
    EXPECT_TRUE(batch.empty());


    EXPECT_THROW(
        queue.waitPopBatch(batch, 0, 10ms),
        std::invalid_argument
    );


    EXPECT_THROW(
        queue.waitPopBatch(batch, 10, -1ms),
        std::invalid_argument
    );
}


// --------------------------------------------------
// 7. 失败记录可以正常传递
// --------------------------------------------------

TEST(BoundedDataQueueTest, FailureRecordCanBeQueuedWithoutValue)
{
    BoundedDataQueue queue(10);

    auto record = makeRecord("failure-001");

    record.devices[0].records.clear();

    record.devices[0].status = CollectionStatus::Failure;

    EXPECT_TRUE(queue.push(std::move(record)));

    std::vector<TelemetryPacket> batch;

    EXPECT_EQ(
        queue.waitPopBatch(batch, 10, 0ms),
        DataQueuePopStatus::DataAvailable
    );

    ASSERT_EQ(batch.size(), 1U);

    EXPECT_TRUE(batch[0].devices[0].records.empty());

    EXPECT_EQ(
        batch[0].devices[0].status,
        CollectionStatus::Failure
    );
}


// ==================================================
// 二、队列生命周期测试
// ==================================================


// --------------------------------------------------
// 8. 关闭队列、拒绝新数据、排空剩余数据
// --------------------------------------------------

TEST(BoundedDataQueueTest, CloseRejectsNewDataAndDrainsRemainingData)
{
    BoundedDataQueue queue(10);

    EXPECT_TRUE(queue.push(makeRecord("message-001")));

    EXPECT_TRUE(queue.push(makeRecord("message-002")));

    queue.close();

    // 重复关闭
    queue.close();

    EXPECT_TRUE(queue.isClosed());

    EXPECT_EQ(queue.size(), 2U);


    // 单条入队应被拒绝，原数据不移动
    auto rejectedRecord = makeRecord("rejected-001");

    EXPECT_FALSE(queue.push(std::move(rejectedRecord)));

    EXPECT_EQ(
        rejectedRecord.message_id,
        "rejected-001"
    );


    // 批量入队也应被拒绝，原数据不移动
    std::vector<TelemetryPacket> rejectedBatch;

    rejectedBatch.emplace_back(
        makeRecord("rejected-002")
    );

    EXPECT_FALSE(
        queue.pushBatch(std::move(rejectedBatch))
    );

    ASSERT_EQ(rejectedBatch.size(), 1U);

    EXPECT_EQ(
        rejectedBatch[0].message_id,
        "rejected-002"
    );


    // 空批次按照接口约定是成功的空操作
    std::vector<TelemetryPacket> emptyBatch;

    EXPECT_TRUE(queue.pushBatch(std::move(emptyBatch)));


    // 关闭后仍然允许消费已有数据
    std::vector<TelemetryPacket> batch;

    EXPECT_EQ(
        queue.waitPopBatch(batch, 10, 0ms),
        DataQueuePopStatus::DataAvailable
    );

    ASSERT_EQ(batch.size(), 2U);

    EXPECT_EQ(batch[0].message_id, "message-001");

    EXPECT_EQ(batch[1].message_id, "message-002");


    // 数据排空后返回 Closed
    EXPECT_EQ(
        queue.waitPopBatch(batch, 10, 0ms),
        DataQueuePopStatus::Closed
    );
}


// ==================================================
// 三、阻塞与背压测试
// ==================================================


// --------------------------------------------------
// 9. 队列满载时，单条生产者阻塞
// --------------------------------------------------

TEST(BoundedDataQueueTest, ProducerBlocksWhenQueueIsFull)
{
    BoundedDataQueue queue(1);

    EXPECT_TRUE(queue.push(makeRecord("message-001")));

    auto pendingRecord = makeRecord("message-002");

    auto future = std::async(
        std::launch::async,
        [&]()
        {
            return queue.push(std::move(pendingRecord));
        }
    );


    // 队列满，生产者应该阻塞
    EXPECT_EQ(
        future.wait_for(50ms),
        std::future_status::timeout
    );


    // 消费一条，释放空间
    std::vector<TelemetryPacket> batch;

    const auto popStatus = queue.waitPopBatch(
        batch,
        1,
        0ms
    );

    EXPECT_EQ(popStatus, DataQueuePopStatus::DataAvailable);

    if (popStatus != DataQueuePopStatus::DataAvailable)
    {
        queue.close();
    }


    // 生产者应该恢复
    const auto futureStatus = future.wait_for(1s);

    EXPECT_EQ(futureStatus, std::future_status::ready);

    if (futureStatus == std::future_status::ready)
    {
        EXPECT_TRUE(future.get());
    }
    else
    {
        queue.close();

        future.wait();

        EXPECT_FALSE(future.get());
    }


    // 取出第二条数据
    if (!queue.isClosed())
    {
        EXPECT_EQ(
            queue.waitPopBatch(batch, 1, 0ms),
            DataQueuePopStatus::DataAvailable
        );

        ASSERT_EQ(batch.size(), 1U);

        EXPECT_EQ(batch[0].message_id, "message-002");
    }
}


// --------------------------------------------------
// 10. 批量入队必须等待足够空间
// --------------------------------------------------

TEST(BoundedDataQueueTest, BatchProducerWaitsForEntireBatchSpace)
{
    BoundedDataQueue queue(5);

    // 先放入4条数据，只剩1个位置
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_TRUE(
            queue.push(
                makeRecord("initial-" + std::to_string(i))
            )
        );
    }


    // 批量提交3条
    std::vector<TelemetryPacket> pendingBatch;

    pendingBatch.emplace_back(makeRecord("batch-001"));

    pendingBatch.emplace_back(makeRecord("batch-002"));

    pendingBatch.emplace_back(makeRecord("batch-003"));


    auto future = std::async(
        std::launch::async,
        [&]()
        {
            return queue.pushBatch(std::move(pendingBatch));
        }
    );


    // 剩余空间不足，必须等待
    EXPECT_EQ(
        future.wait_for(50ms),
        std::future_status::timeout
    );

    EXPECT_EQ(queue.size(), 4U);


    std::vector<TelemetryPacket> batch;


    // 消费一条，现在只剩2个空位
    EXPECT_EQ(
        queue.waitPopBatch(batch, 1, 0ms),
        DataQueuePopStatus::DataAvailable
    );

    EXPECT_EQ(queue.size(), 3U);


    // 仍然不足以容纳3条数据
    EXPECT_EQ(
        future.wait_for(50ms),
        std::future_status::timeout
    );


    // 再消费一条，空出3个位置
    EXPECT_EQ(
        queue.waitPopBatch(batch, 1, 0ms),
        DataQueuePopStatus::DataAvailable
    );

    EXPECT_EQ(queue.size(), 2U);


    const auto futureStatus = future.wait_for(1s);

    EXPECT_EQ(futureStatus, std::future_status::ready);

    if (futureStatus == std::future_status::ready)
    {
        EXPECT_TRUE(future.get());
    }
    else
    {
        queue.close();

        future.wait();

        EXPECT_FALSE(future.get());
    }


    // 批次提交后，队列应该正好有5条数据
    if (!queue.isClosed())
    {
        EXPECT_EQ(queue.size(), 5U);

        EXPECT_EQ(
            queue.waitPopBatch(batch, 10, 0ms),
            DataQueuePopStatus::DataAvailable
        );

        ASSERT_EQ(batch.size(), 5U);

        EXPECT_EQ(batch[0].message_id, "initial-2");

        EXPECT_EQ(batch[1].message_id, "initial-3");

        EXPECT_EQ(batch[2].message_id, "batch-001");

        EXPECT_EQ(batch[3].message_id, "batch-002");

        EXPECT_EQ(batch[4].message_id, "batch-003");
    }
}


// --------------------------------------------------
// 11. 关闭队列能够唤醒阻塞生产者
// --------------------------------------------------

TEST(BoundedDataQueueTest, CloseWakesBlockedProducer)
{
    BoundedDataQueue queue(1);

    EXPECT_TRUE(queue.push(makeRecord("message-001")));

    auto pendingRecord = makeRecord("message-002");

    auto future = std::async(
        std::launch::async,
        [&]()
        {
            return queue.push(std::move(pendingRecord));
        }
    );


    EXPECT_EQ(
        future.wait_for(50ms),
        std::future_status::timeout
    );


    queue.close();


    const auto futureStatus = future.wait_for(1s);

    EXPECT_EQ(futureStatus, std::future_status::ready);

    if (futureStatus == std::future_status::ready)
    {
        EXPECT_FALSE(future.get());

        EXPECT_EQ(
            pendingRecord.message_id,
            "message-002"
        );
    }
    else
    {
        queue.close();

        future.wait();

        EXPECT_FALSE(future.get());
    }
}


// --------------------------------------------------
// 12. 关闭队列能够唤醒阻塞消费者
// --------------------------------------------------

TEST(BoundedDataQueueTest, CloseWakesBlockedConsumer)
{
    BoundedDataQueue queue(10);

    auto future = std::async(
        std::launch::async,
        [&]()
        {
            std::vector<TelemetryPacket> batch;

            return queue.waitPopBatch(
                batch,
                10,
                5s
            );
        }
    );


    // 消费者正在等待数据
    EXPECT_EQ(
        future.wait_for(50ms),
        std::future_status::timeout
    );


    queue.close();


    const auto futureStatus = future.wait_for(1s);

    EXPECT_EQ(futureStatus, std::future_status::ready);

    if (futureStatus == std::future_status::ready)
    {
        EXPECT_EQ(
            future.get(),
            DataQueuePopStatus::Closed
        );
    }
    else
    {
        future.wait();
    }
}


// --------------------------------------------------
// 13. 慢消费者场景：模拟真实背压
// --------------------------------------------------

TEST(BoundedDataQueueTest, BackpressureWithSlowConsumer)
{
    constexpr std::size_t CAPACITY = 8;

    constexpr std::size_t BATCH_SIZE = 4;

    constexpr std::size_t TOTAL_BATCHES = 50;

    constexpr std::size_t TOTAL_RECORDS =
        BATCH_SIZE * TOTAL_BATCHES;


    BoundedDataQueue queue(CAPACITY);

    std::atomic<bool> producerFinished{false};

    std::atomic<bool> producerSuccess{true};

    std::atomic<std::size_t> consumed{0};


    // ==================================================
    // 快速生产者
    // ==================================================

    auto producerFuture = std::async(
        std::launch::async,
        [&]()
        {
            for (std::size_t i = 0; i < TOTAL_BATCHES; ++i)
            {
                std::vector<TelemetryPacket> records;

                records.reserve(BATCH_SIZE);

                for (std::size_t j = 0; j < BATCH_SIZE; ++j)
                {
                    const std::size_t id =
                        i * BATCH_SIZE + j;

                    records.emplace_back(
                        makeRecord(
                            "message-" + std::to_string(id)
                        )
                    );
                }

                if (!queue.pushBatch(std::move(records)))
                {
                    producerSuccess.store(false);

                    break;
                }
            }

            producerFinished.store(true);
        }
    );


    // 等待队列被生产者填满
    const auto deadline =
        std::chrono::steady_clock::now() + 1s;

    while (queue.size() < CAPACITY &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_EQ(queue.size(), CAPACITY);


    // 队列满后，生产者应当因为背压而阻塞
    EXPECT_EQ(
        producerFuture.wait_for(50ms),
        std::future_status::timeout
    );

    EXPECT_FALSE(producerFinished.load());


    // ==================================================
    // 启动慢消费者
    // ==================================================

    std::thread consumer(
        [&]()
        {
            std::vector<TelemetryPacket> batch;

            while (true)
            {
                const auto status = queue.waitPopBatch(
                    batch,
                    BATCH_SIZE,
                    100ms
                );

                if (status == DataQueuePopStatus::DataAvailable)
                {
                    consumed.fetch_add(
                        batch.size(),
                        std::memory_order_relaxed
                    );

                    // 模拟 StorageWorker 写入 SQLite 的耗时
                    std::this_thread::sleep_for(2ms);
                }
                else if (status == DataQueuePopStatus::Closed)
                {
                    break;
                }
            }
        }
    );


    // 消费者启动后，生产者应该能够继续完成
    producerFuture.get();

    EXPECT_TRUE(producerSuccess.load());

    EXPECT_TRUE(producerFinished.load());


    // 所有生产者结束后关闭队列
    queue.close();

    consumer.join();


    // 最终验证没有数据丢失
    EXPECT_EQ(
        consumed.load(),
        TOTAL_RECORDS
    );

    EXPECT_EQ(queue.size(), 0U);
}


// ==================================================
// 四、并发安全测试
// ==================================================


// --------------------------------------------------
// 14. 多生产者 + 单消费者，数据完整性测试
// --------------------------------------------------

TEST(BoundedDataQueueTest, MultipleProducersSingleConsumerNoDataLoss)
{
    constexpr std::size_t PRODUCER_COUNT = 4;

    constexpr std::size_t RECORDS_PER_PRODUCER = 5000;

    constexpr std::size_t TOTAL_RECORDS =
        PRODUCER_COUNT * RECORDS_PER_PRODUCER;


    BoundedDataQueue queue(256);

    std::atomic<bool> producerFailed{false};

    std::atomic<bool> duplicateFound{false};

    std::atomic<bool> capacityViolation{false};

    std::atomic<bool> monitoring{true};

    std::atomic<std::size_t> consumed{0};


    std::unordered_set<std::string> messageIds;


    // ==================================================
    // 单消费者线程
    // ==================================================

    std::thread consumer(
        [&]()
        {
            std::vector<TelemetryPacket> batch;

            while (true)
            {
                const auto status = queue.waitPopBatch(
                    batch,
                    128,
                    100ms
                );

                if (status == DataQueuePopStatus::DataAvailable)
                {
                    for (const auto& record : batch)
                    {
                        if (!messageIds.insert(record.message_id).second)
                        {
                            duplicateFound.store(true);
                        }
                    }

                    consumed.fetch_add(
                        batch.size(),
                        std::memory_order_relaxed
                    );
                }
                else if (status == DataQueuePopStatus::Closed)
                {
                    break;
                }
            }
        }
    );


    // ==================================================
    // 并发状态查询线程
    // ==================================================

    std::thread monitor(
        [&]()
        {
            while (monitoring.load())
            {
                if (queue.size() > queue.capacity())
                {
                    capacityViolation.store(true);
                }

                (void)queue.isClosed();

                std::this_thread::yield();
            }
        }
    );


    // ==================================================
    // 多生产者线程
    // ==================================================

    std::vector<std::thread> producers;

    for (std::size_t producerId = 0;
         producerId < PRODUCER_COUNT;
         ++producerId)
    {
        producers.emplace_back(
            [&, producerId]()
            {
                const std::int64_t deviceId = static_cast<std::int64_t>(producerId);

                for (std::size_t i = 0;
                     i < RECORDS_PER_PRODUCER;
                     ++i)
                {
                    auto record = makeRecord(
                        std::to_string(deviceId) + "-" + std::to_string(i),
                        deviceId,
                        "temperature",
                        static_cast<std::int64_t>(i)
                    );

                    if (!queue.push(std::move(record)))
                    {
                        producerFailed.store(true);

                        return;
                    }
                }
            }
        );
    }


    // 等待生产者结束
    for (auto& producer : producers)
    {
        producer.join();
    }


    queue.close();

    consumer.join();


    monitoring.store(false);

    monitor.join();


    // ==================================================
    // 验证
    // ==================================================

    EXPECT_FALSE(producerFailed.load());

    EXPECT_FALSE(duplicateFound.load());

    EXPECT_FALSE(capacityViolation.load());

    EXPECT_EQ(consumed.load(), TOTAL_RECORDS);

    EXPECT_EQ(messageIds.size(), TOTAL_RECORDS);

    EXPECT_EQ(queue.size(), 0U);
}


// --------------------------------------------------
// 15. 并发批量生产者：批次不能相互交叉
// --------------------------------------------------

TEST(BoundedDataQueueTest, ConcurrentBatchProducersPreserveBatchContiguity)
{
    constexpr std::size_t PRODUCER_COUNT = 4;

    constexpr std::size_t BATCH_SIZE = 50;

    constexpr std::size_t TOTAL_RECORDS =
        PRODUCER_COUNT * BATCH_SIZE;


    BoundedDataQueue queue(TOTAL_RECORDS);

    std::atomic<bool> producerFailed{false};

    std::vector<std::thread> producers;


    for (std::size_t producerId = 0;
         producerId < PRODUCER_COUNT;
         ++producerId)
    {
        producers.emplace_back(
            [&, producerId]()
            {
                const std::int64_t deviceId = static_cast<std::int64_t>(producerId);

                std::vector<TelemetryPacket> records;

                records.reserve(BATCH_SIZE);

                for (std::size_t i = 0; i < BATCH_SIZE; ++i)
                {
                    records.emplace_back(
                        makeRecord(
                            std::to_string(deviceId) + "-" + std::to_string(i),
                            deviceId,
                            std::to_string(i)
                        )
                    );
                }

                if (!queue.pushBatch(std::move(records)))
                {
                    producerFailed.store(true);
                }
            }
        );
    }


    for (auto& producer : producers)
    {
        producer.join();
    }


    queue.close();


    std::vector<TelemetryPacket> records;

    const auto status = queue.waitPopBatch(
        records,
        TOTAL_RECORDS,
        0ms
    );

    EXPECT_EQ(status, DataQueuePopStatus::DataAvailable);

    EXPECT_FALSE(producerFailed.load());

    ASSERT_EQ(records.size(), TOTAL_RECORDS);


    // ==================================================
    // 验证每个生产者的批次是否连续
    // ==================================================

    std::size_t batchCount = 0;

    std::size_t currentBatchSize = 0;

    std::int64_t currentProducer = -1;


    for (const auto& record : records)
    {
        if (record.devices[0].device_id != currentProducer)
        {
            if (currentProducer != -1)
            {
                EXPECT_EQ(currentBatchSize, BATCH_SIZE);
            }

            currentProducer = record.devices[0].device_id;

            currentBatchSize = 0;

            ++batchCount;
        }

        // 验证批次内部顺序
        EXPECT_EQ(
            record.devices[0].records[0].property,
            std::to_string(currentBatchSize)
        );

        ++currentBatchSize;
    }


    EXPECT_EQ(currentBatchSize, BATCH_SIZE);

    EXPECT_EQ(batchCount, PRODUCER_COUNT);
}


// ==================================================
// 五、性能测试
// ==================================================


// --------------------------------------------------
// 16. 不同容量下的吞吐量对比
// --------------------------------------------------

TEST(BoundedDataQueuePerformanceTest, ThroughputAcrossCapacities)
{
    const std::array<std::size_t, 4> capacities = {
        500,
        2000,
        5000,
        10000
    };


    double bestSinglePush = 0.0;

    double bestBatchPush = 0.0;

    std::size_t bestSingleCapacity = 0;

    std::size_t bestBatchCapacity = 0;


    std::cout << "\n";

    std::cout << "==================================================\n";

    std::cout << "       BoundedDataQueue Performance Test\n";

    std::cout << "==================================================\n";


    for (const auto capacity : capacities)
    {
        std::cout << "\nQueue Capacity: "
                  << capacity
                  << "\n";


        // ------------------------------------------
        // 单条入队性能
        // ------------------------------------------

        const auto singleResult = runQueueBenchmark(
            capacity,
            false
        );

        EXPECT_FALSE(singleResult.producer_failed);

        EXPECT_EQ(
            singleResult.consumed,
            singleResult.produced
        );

        EXPECT_GT(singleResult.throughput, 0.0);


        std::cout << "[Single Push]\n";

        std::cout << "Produced: "
                  << singleResult.produced
                  << "\n";

        std::cout << "Consumed: "
                  << singleResult.consumed
                  << "\n";

        std::cout << "Elapsed: "
                  << std::fixed
                  << std::setprecision(2)
                  << singleResult.elapsed_ms
                  << " ms\n";

        std::cout << "Throughput: "
                  << std::fixed
                  << std::setprecision(0)
                  << singleResult.throughput
                  << " records/s\n";


        if (singleResult.throughput > bestSinglePush)
        {
            bestSinglePush = singleResult.throughput;

            bestSingleCapacity = capacity;
        }


        // ------------------------------------------
        // 批量入队性能
        // ------------------------------------------

        const auto batchResult = runQueueBenchmark(
            capacity,
            true
        );

        EXPECT_FALSE(batchResult.producer_failed);

        EXPECT_EQ(
            batchResult.consumed,
            batchResult.produced
        );

        EXPECT_GT(batchResult.throughput, 0.0);


        std::cout << "[Batch Push]\n";

        std::cout << "Produced: "
                  << batchResult.produced
                  << "\n";

        std::cout << "Consumed: "
                  << batchResult.consumed
                  << "\n";

        std::cout << "Elapsed: "
                  << std::fixed
                  << std::setprecision(2)
                  << batchResult.elapsed_ms
                  << " ms\n";

        std::cout << "Throughput: "
                  << std::fixed
                  << std::setprecision(0)
                  << batchResult.throughput
                  << " records/s\n";


        if (batchResult.throughput > bestBatchPush)
        {
            bestBatchPush = batchResult.throughput;

            bestBatchCapacity = capacity;
        }
    }


    // ==================================================
    // 性能测试汇总
    // ==================================================

    std::cout << "\n";

    std::cout << "==================================================\n";

    std::cout << "                 Benchmark Summary\n";

    std::cout << "==================================================\n";


    std::cout << "Best Single Push Throughput: "
              << bestSinglePush
              << " records/s, Capacity: "
              << bestSingleCapacity
              << "\n";


    std::cout << "Best Batch Push Throughput: "
              << bestBatchPush
              << " records/s, Capacity: "
              << bestBatchCapacity
              << "\n";


    std::cout << "==================================================\n";
}
