#include "MqttPublisherWorker.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace industrial_gateway;
using namespace application::ports;
using application::publishing::MqttPublisherWorker;
using namespace std::chrono_literals;

struct State
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<StoredMessage> rows;
    std::vector<std::string> payloads;
    std::vector<std::string> topics;
    std::vector<std::chrono::milliseconds> timeouts;
    std::vector<std::chrono::steady_clock::time_point> publish_times;
    std::vector<std::vector<std::int64_t>> deletions;
    std::vector<std::size_t> query_limits;
    std::deque<MqttPublishStatus> results;
    int initialize_calls = 0;
    int failed_initializations = 0;
    int failed_queries = 0;
    int failed_deletions = 0;
    int publish_exceptions = 0;
    int critical_logs = 0;
    bool connected = true;
    bool block_publish = false;
    bool release_publish = false;
    bool throw_logs = false;

    template<class Predicate>
    bool wait(Predicate predicate)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 6s, predicate);
    }
};

class FakeStorage final : public IMessageStorage
{
public:
    explicit FakeStorage(State& state) : s(state) {}
    bool initialize(std::string& error) override
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.initialize_calls;
        s.changed.notify_all();
        if (s.failed_initializations > 0)
        {
            --s.failed_initializations;
            error = "initialization failure";
            return false;
        }
        return true;
    }
    bool insertBatch(const std::vector<MessageToStore>&, std::string&) override
    {
        ADD_FAILURE() << "Publisher must not insert messages";
        return false;
    }
    bool queryBatch(std::size_t limit, std::vector<StoredMessage>& out, std::string& error) override
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.query_limits.push_back(limit);
        out.clear();
        s.changed.notify_all();
        if (s.failed_queries > 0)
        {
            --s.failed_queries;
            error = "query failure";
            return false;
        }
        out.assign(s.rows.begin(), s.rows.begin() + std::min(limit, s.rows.size()));
        return true;
    }
    bool deleteBatch(const std::vector<std::int64_t>& ids, std::string& error) override
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.deletions.push_back(ids);
        s.changed.notify_all();
        if (s.failed_deletions > 0)
        {
            --s.failed_deletions;
            error = "delete failure";
            return false;
        }
        s.rows.erase(std::remove_if(s.rows.begin(), s.rows.end(), [&](const auto& row)
        {
            return std::find(ids.begin(), ids.end(), row.id) != ids.end();
        }), s.rows.end());
        return true;
    }
private:
    State& s;
};

class FakeMqtt final : public IMqttClient
{
public:
    explicit FakeMqtt(State& state) : s(state) {}
    bool start(std::string&) override
    {
        ADD_FAILURE() << "Worker must not start MQTT";
        return false;
    }
    void stop() noexcept override { ADD_FAILURE() << "Worker must not stop MQTT"; }
    bool isConnected() const noexcept override
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.connected;
    }
    MqttPublishResult publish(const std::string& topic, const std::string& payload,
                              std::chrono::milliseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        s.topics.push_back(topic);
        s.payloads.push_back(payload);
        s.timeouts.push_back(timeout);
        s.publish_times.push_back(std::chrono::steady_clock::now());
        s.changed.notify_all();
        if (s.publish_exceptions > 0)
        {
            --s.publish_exceptions;
            throw std::runtime_error("publish exception");
        }
        if (s.block_publish)
        {
            // 有界等待，测试断言失败也不会使析构永久阻塞。
            s.changed.wait_for(lock, 3s, [&]() { return s.release_publish; });
        }
        const auto result = s.results.empty() ? MqttPublishStatus::Acknowledged : s.results.front();
        if (!s.results.empty()) s.results.pop_front();
        return {result, "simulated result"};
    }
    bool subscribe(const std::string&, int, MqttMessageHandler,
                   MqttSubscriptionResultHandler, std::string&) override
    {
        ADD_FAILURE() << "Telemetry publisher must not subscribe";
        return false;
    }
private:
    State& s;
};

class FakeLogger final : public common::ILogger
{
public:
    explicit FakeLogger(State& state) : s(state) {}
    void log(common::LogLevel level, std::string_view, std::string_view) override
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (level == common::LogLevel::Critical) ++s.critical_logs;
        s.changed.notify_all();
        if (s.throw_logs) throw std::runtime_error("logger failure");
    }
private:
    State& s;
};

class MqttPublisherWorkerTest : public testing::Test
{
protected:
    State s;
    FakeStorage storage{s};
    FakeMqtt mqtt{s};
    FakeLogger logger{s};
    MqttPublisherWorker worker{storage, mqtt, logger, "gateway-001"};
    std::string error;
};

TEST_F(MqttPublisherWorkerTest, PublishesRawPayloadWithTelemetryTopicAndDeletesInBatches)
{
    for (int i = 1; i <= 205; ++i) s.rows.push_back({i, 1000, "packet-" + std::to_string(i)});
    s.rows[0].message = std::string("raw\0payload", 11);
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    ASSERT_EQ(s.payloads.size(), 205U);
    EXPECT_EQ(s.payloads[0], std::string("raw\0payload", 11));
    for (std::size_t i = 1; i < s.payloads.size(); ++i)
        EXPECT_EQ(s.payloads[i], "packet-" + std::to_string(i + 1));
    for (const auto& topic : s.topics) EXPECT_EQ(topic, "v1/gateways/gateway-001/telemetry");
    for (auto timeout : s.timeouts) EXPECT_EQ(timeout, 5s);
    for (auto limit : s.query_limits) EXPECT_EQ(limit, 100U);
    ASSERT_EQ(s.deletions.size(), 3U);
    EXPECT_EQ(s.deletions[0].size(), 100U);
    EXPECT_EQ(s.deletions[1].size(), 100U);
    EXPECT_EQ(s.deletions[2].size(), 5U);
}

class PublishFailureTest : public MqttPublisherWorkerTest,
                           public testing::WithParamInterface<MqttPublishStatus> {};

TEST_P(PublishFailureTest, KeepsFailedRecordStopsBatchAndRetriesAfterDelay)
{
    s.rows = {{11, 1000, "A"}, {22, 1000, "B"}, {33, 1000, "C"}};
    s.results = {MqttPublishStatus::Acknowledged, GetParam()};
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    EXPECT_EQ(s.payloads, (std::vector<std::string>{"A", "B", "B", "C"}));
    EXPECT_EQ(s.deletions, (std::vector<std::vector<std::int64_t>>{{11}, {22, 33}}));
    ASSERT_EQ(s.publish_times.size(), 4U);
    EXPECT_GE(s.publish_times[2] - s.publish_times[1], 1s);
}

INSTANTIATE_TEST_SUITE_P(Unacknowledged, PublishFailureTest, testing::Values(
    MqttPublishStatus::AcknowledgementTimeout, MqttPublishStatus::ConnectionLost,
    MqttPublishStatus::NotConnected, MqttPublishStatus::PublishFailed,
    MqttPublishStatus::InternalError));

TEST_F(MqttPublisherWorkerTest, RetriesDeletionBeforePublishingAnotherBatch)
{
    for (int i = 1; i <= 101; ++i) s.rows.push_back({i, 1000, std::to_string(i)});
    s.failed_deletions = 1;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return !s.deletions.empty(); }));
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        EXPECT_EQ(s.payloads.size(), 100U);
        EXPECT_EQ(s.query_limits.size(), 1U);
        EXPECT_EQ(s.rows.size(), 101U);
    }
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    EXPECT_EQ(s.payloads.size(), 101U);
    ASSERT_EQ(s.deletions.size(), 3U);
    EXPECT_EQ(s.deletions[0], s.deletions[1]);
    EXPECT_GE(s.initialize_calls, 2);
}

TEST_F(MqttPublisherWorkerTest, RecoversInitializationAndQueryFailure)
{
    s.failed_initializations = 1;
    s.failed_queries = 1;
    s.rows = {{1, 1000, "A"}};
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    EXPECT_GE(s.initialize_calls, 3);
    EXPECT_EQ(s.payloads, (std::vector<std::string>{"A"}));
}

TEST_F(MqttPublisherWorkerTest, WaitsForConnectionWithoutReadingAndStopWakesRetry)
{
    s.connected = false;
    s.rows = {{1, 1000, "A"}};
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.initialize_calls > 0; }));
    const auto before = std::chrono::steady_clock::now();
    worker.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
    EXPECT_TRUE(s.query_limits.empty());
    EXPECT_TRUE(s.payloads.empty());
    s.connected = true;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    EXPECT_EQ(s.payloads.size(), 1U);
}

TEST_F(MqttPublisherWorkerTest, StopInterruptsPersistentTimeoutRetryWithoutDeleting)
{
    s.rows = {{1, 1000, "A"}, {2, 1000, "B"}};
    s.results.assign(100, MqttPublishStatus::AcknowledgementTimeout);
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return !s.payloads.empty(); }));
    worker.stop();
    EXPECT_EQ(s.payloads, (std::vector<std::string>{"A"}));
    EXPECT_TRUE(s.deletions.empty());
    EXPECT_EQ(s.rows.size(), 2U);
}

TEST_F(MqttPublisherWorkerTest, StopExitsOnDeleteFailureAndRestartMayRepublish)
{
    s.rows = {{1, 1000, "A"}};
    s.failed_deletions = 100;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return !s.deletions.empty(); }));
    worker.stop();
    ASSERT_EQ(s.rows.size(), 1U);
    s.failed_deletions = 0;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
    EXPECT_EQ(s.payloads, (std::vector<std::string>{"A", "A"}));
}

TEST_F(MqttPublisherWorkerTest, StopWaitsForActivePublishThenDeletesOnlyItsAck)
{
    s.rows = {{1, 1000, "A"}, {2, 1000, "B"}};
    s.block_publish = true;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.payloads.size() == 1; }));
    auto stopping = std::async(std::launch::async, [&]() { worker.stop(); });
    EXPECT_EQ(stopping.wait_for(100ms), std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        EXPECT_EQ(s.payloads.size(), 1U);
        EXPECT_TRUE(s.deletions.empty());
        s.release_publish = true;
        s.changed.notify_all();
    }
    stopping.get();
    EXPECT_EQ(s.deletions, (std::vector<std::vector<std::int64_t>>{{1}}));
    ASSERT_EQ(s.rows.size(), 1U);
    EXPECT_EQ(s.rows[0].id, 2);
}

TEST_F(MqttPublisherWorkerTest, LifecycleIsIdempotentAndEmptyStorageWaitIsInterruptible)
{
    worker.stop();
    error = "old error";
    ASSERT_TRUE(worker.start(error));
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return !s.query_limits.empty(); }));
    worker.stop();
    worker.stop();
    EXPECT_EQ(s.initialize_calls, 1);
    EXPECT_TRUE(s.payloads.empty());
    EXPECT_TRUE(s.deletions.empty());
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.initialize_calls == 2; }));
    worker.stop();
}

TEST_F(MqttPublisherWorkerTest, CatchesPublishExceptionAndRetainsRecordForRestart)
{
    s.rows = {{1, 1000, "A"}};
    s.publish_exceptions = 1;
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.critical_logs == 1; }));
    worker.stop();
    EXPECT_TRUE(s.deletions.empty());
    EXPECT_EQ(s.rows.size(), 1U);
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
}

TEST_F(MqttPublisherWorkerTest, LoggingExceptionsDoNotPreventAckDeletion)
{
    s.throw_logs = true;
    s.rows = {{1, 1000, "A"}};
    ASSERT_TRUE(worker.start(error));
    ASSERT_TRUE(s.wait([&]() { return s.rows.empty(); }));
    worker.stop();
}

TEST_F(MqttPublisherWorkerTest, RejectsInvalidGatewayIds)
{
    for (const auto& id : std::vector<std::string>{"", "a/b", "a+b", "a#b", std::string("a\0b", 3)})
    {
        EXPECT_THROW((MqttPublisherWorker{storage, mqtt, logger, id}), std::invalid_argument);
    }
}

TEST_F(MqttPublisherWorkerTest, DestructorStopsAnActiveWorker)
{
    s.failed_initializations = 100;
    {
        MqttPublisherWorker scoped_worker(storage, mqtt, logger, "gateway-002");
        ASSERT_TRUE(scoped_worker.start(error));
        ASSERT_TRUE(s.wait([&]() { return s.initialize_calls > 0; }));
    }
    EXPECT_TRUE(s.payloads.empty());
}
} // namespace
