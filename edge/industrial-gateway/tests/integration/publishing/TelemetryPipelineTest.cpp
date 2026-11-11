// 真实 Broker 集成测试，默认禁用。运行方式见同目录 README.md。
#include "StorageWorker.hpp"
#include "MqttPublisherWorker.hpp"
#include "BoundedDataQueue.hpp"
#include "SQLiteMessageStorage.hpp"
#include "MosquittoMqttClient.hpp"
#include "MqttTopics.hpp"
#include "industrial_gateway/domain/model/TelemetryPacket.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace industrial_gateway;
using namespace application::ports;
using application::ingestion::StorageWorker;
using application::publishing::MqttPublisherWorker;
using application::messaging::MqttTopics;
using infrastructure::concurrency::BoundedDataQueue;
using infrastructure::storage::sqlite::SQLiteMessageStorage;
using infrastructure::mqtt::MosquittoMqttClient;
using domain::model::TelemetryPacket;
using domain::enums::CollectionStatus;
using namespace std::chrono_literals;

class TestLogger final : public common::ILogger
{
public:
    void log(common::LogLevel level, std::string_view component, std::string_view message) override
    {
        if (level < common::LogLevel::Warn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        std::cerr << '[' << component << "] " << message << '\n';
    }
private:
    std::mutex mutex_;
};

template<class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 15s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "gateway-pipeline-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* result = ::mkdtemp(writable.data());
        if (!result) throw std::runtime_error("Cannot create isolated test directory");
        path = result;
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

// 独立 MQTT 客户端的收件箱，回调中只复制消息，不执行测试断言。
struct Inbox
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<MqttMessage> messages;
    std::vector<MqttSubscriptionResult> subscriptions;

    bool waitMessages(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 20s, [&]() { return messages.size() >= count; });
    }
    std::vector<MqttMessage> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return messages;
    }
};

// 使用所有真实组件；两个 Worker 和测试观察者各有独立 SQLite 连接。
class Pipeline
{
public:
    Pipeline(const config::StorageConfig& db, const config::MqttConfig& mqtt_config,
             common::ILogger& logger, const std::string& gateway_id)
        : writer(db), reader(db), mqtt(mqtt_config, logger),
          storage_worker(queue, writer, logger), publisher(reader, mqtt, logger, gateway_id)
    {
        storage_thread = std::thread([this]()
        {
            try { storage_worker.run(); }
            catch (...) { storage_exception = std::current_exception(); }
        });
    }
    ~Pipeline()
    {
        queue.close();
        if (storage_thread.joinable()) storage_thread.join();
        publisher.stop();
        mqtt.stop();
    }
    void finishIngestion()
    {
        queue.close();
        if (storage_thread.joinable()) storage_thread.join();
        if (storage_exception) std::rethrow_exception(storage_exception);
    }
    BoundedDataQueue queue{512};
    SQLiteMessageStorage writer;
    SQLiteMessageStorage reader;
    MosquittoMqttClient mqtt;
    StorageWorker storage_worker;
    MqttPublisherWorker publisher;
private:
    std::exception_ptr storage_exception;
    std::thread storage_thread;
};

TelemetryPacket makePacket(const std::string& id, std::int64_t timestamp = 1794000000000)
{
    return {id, timestamp, {
        {10001, CollectionStatus::Success,
            {{"temperature", 36.5}, {"speed", std::int64_t{1500}}}},
        {10002, CollectionStatus::Success, {{"current", 12.5}}},
        {10003, CollectionStatus::Failure, {}}
    }};
}

class TelemetryPipelineTest : public testing::Test
{
protected:
    void SetUp() override
    {
        const char* path = std::getenv("MQTT_TEST_CONFIG");
        const auto document = YAML::LoadFile(path ? path : "config/gateway.yaml");
        const auto node = document["mqtt"];
        ASSERT_TRUE(node.IsMap());
        mqtt_config.enabled = node["enabled"].as<bool>();
        mqtt_config.host = node["host"].as<std::string>();
        mqtt_config.port = node["port"].as<int>();
        mqtt_config.keep_alive = node["keep_alive"].as<int>();
        ASSERT_TRUE(mqtt_config.enabled) << "MQTT must be enabled in the test configuration";
        gateway_id = "integration-" + std::to_string(::getpid()) + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        mqtt_config.client_id = gateway_id + "-publisher";
        topic = MqttTopics::telemetry(gateway_id);
        db_config.database_path = (temporary.path / "messages.db").string();
        observer = std::make_unique<SQLiteMessageStorage>(db_config);
        ASSERT_TRUE(observer->initialize(error)) << error;

        auto subscriber_config = mqtt_config;
        subscriber_config.client_id = gateway_id + "-subscriber";
        subscriber = std::make_unique<MosquittoMqttClient>(subscriber_config, logger);
        ASSERT_TRUE(subscriber->subscribe(topic, 1,
            [mailbox = inbox](const MqttMessage& message)
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                mailbox->messages.push_back(message);
                mailbox->changed.notify_all();
            },
            [mailbox = inbox](const MqttSubscriptionResult& result)
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                mailbox->subscriptions.push_back(result);
                mailbox->changed.notify_all();
            }, error)) << error;
        std::cout << "\nBroker: " << mqtt_config.host << ':' << mqtt_config.port
                  << "\nSubscribed topic: " << topic << '\n';
        ASSERT_TRUE(subscriber->start(error)) << error;
        std::unique_lock<std::mutex> lock(inbox->mutex);
        ASSERT_TRUE(inbox->changed.wait_for(lock, 15s,
            [&]() { return !inbox->subscriptions.empty(); }))
            << "Subscriber did not receive SUBACK; check Broker/network access";
        ASSERT_TRUE(inbox->subscriptions.back().isAcknowledged());
    }

    void TearDown() override
    {
        if (subscriber) subscriber->stop();
    }

    std::vector<StoredMessage> rows()
    {
        std::vector<StoredMessage> result;
        if (!observer->queryBatch(10000, result, error)) throw std::runtime_error(error);
        return result;
    }

    void checkReceived(std::size_t count)
    {
        const auto messages = inbox->snapshot();
        ASSERT_EQ(messages.size(), count);
        for (const auto& message : messages)
        {
            EXPECT_EQ(message.topic, topic);
            EXPECT_EQ(message.qos, 1);
            EXPECT_FALSE(message.retained);
        }
        if (!messages.empty())
        {
            std::cout << "[subscriber] " << messages.front().topic << '\n'
                      << messages.front().payload << '\n'
                      << "Received " << messages.size() << " packets; SQLite pending: "
                      << rows().size() << '\n';
        }
    }

    TemporaryDirectory temporary;
    TestLogger logger;
    config::StorageConfig db_config;
    config::MqttConfig mqtt_config;
    std::string gateway_id;
    std::string topic;
    std::string error;
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    std::unique_ptr<SQLiteMessageStorage> observer;
    std::unique_ptr<MosquittoMqttClient> subscriber;
};

TEST_F(TelemetryPipelineTest, DISABLED_MultiDevicePacketRoundTrip)
{
    Pipeline gateway(db_config, mqtt_config, logger, gateway_id);
    // 先持久化并快照，再发布，以逐字节验证 SQLite 与订阅端内容相同。
    ASSERT_TRUE(gateway.queue.push(makePacket("packet-001")));
    gateway.finishIngestion();
    const auto persisted = rows();
    ASSERT_EQ(persisted.size(), 1U);
    EXPECT_EQ(persisted[0].timestamp_ms, 1794000000000);
    EXPECT_EQ(persisted[0].message,
        R"({"message_id":"packet-001","timestamp_ms":1794000000000,"devices":[{"device_id":10001,"status":"Success","records":[{"property":"temperature","value":36.5},{"property":"speed","value":1500}]},{"device_id":10002,"status":"Success","records":[{"property":"current","value":12.5}]},{"device_id":10003,"status":"Failure","records":[]}]})");
    ASSERT_TRUE(gateway.mqtt.start(error)) << error;
    ASSERT_TRUE(gateway.publisher.start(error)) << error;
    ASSERT_TRUE(inbox->waitMessages(1));
    ASSERT_TRUE(waitUntil([&]() { return rows().empty(); }));
    gateway.publisher.stop();
    checkReceived(1);
    EXPECT_EQ(inbox->snapshot()[0].payload, persisted[0].message);
}

TEST_F(TelemetryPipelineTest, DISABLED_ConcurrentIngestionAndPublishingAcrossBatches)
{
    Pipeline gateway(db_config, mqtt_config, logger, gateway_id);
    ASSERT_TRUE(gateway.mqtt.start(error)) << error;
    ASSERT_TRUE(waitUntil([&]() { return gateway.mqtt.isConnected(); }));
    ASSERT_TRUE(gateway.publisher.start(error)) << error;
    // 超过单次查询上限，同时运行写入线程和发布线程。
    constexpr std::size_t count = 205;
    for (std::size_t i = 0; i < count; ++i)
        ASSERT_TRUE(gateway.queue.push(makePacket("packet-" + std::to_string(i), 1000 + i)));
    gateway.finishIngestion();
    ASSERT_TRUE(inbox->waitMessages(count));
    ASSERT_TRUE(waitUntil([&]() { return rows().empty(); }));
    gateway.publisher.stop();
    checkReceived(count);
    const auto messages = inbox->snapshot();
    ASSERT_EQ(messages.size(), count);
    for (std::size_t i = 0; i < count; ++i)
    {
        // yaml-cpp 能读取 JSON；无需给生产代码引入新的 JSON 库。
        const auto packet = YAML::Load(messages[i].payload);
        EXPECT_EQ(packet["message_id"].as<std::string>(), "packet-" + std::to_string(i));
        EXPECT_EQ(packet["timestamp_ms"].as<std::int64_t>(), 1000 + i);
        ASSERT_EQ(packet["devices"].size(), 3U);
    }
}

TEST_F(TelemetryPipelineTest, DISABLED_ClientDisconnectBuffersAndReconnectDrains)
{
    Pipeline gateway(db_config, mqtt_config, logger, gateway_id);
    ASSERT_TRUE(gateway.mqtt.start(error)) << error;
    ASSERT_TRUE(gateway.publisher.start(error)) << error;
    ASSERT_TRUE(gateway.queue.push(makePacket("before-disconnect")));
    ASSERT_TRUE(inbox->waitMessages(1));
    ASSERT_TRUE(waitUntil([&]() { return rows().empty(); }));

    // 只停止测试发布客户端，不停止共享 Broker 或订阅端。
    gateway.mqtt.stop();
    ASSERT_FALSE(gateway.mqtt.isConnected());
    ASSERT_TRUE(gateway.queue.push(makePacket("offline-1")));
    ASSERT_TRUE(gateway.queue.push(makePacket("offline-2")));
    gateway.finishIngestion();
    const auto offline = rows();
    ASSERT_EQ(offline.size(), 2U);
    EXPECT_EQ(inbox->snapshot().size(), 1U);
    // PublisherWorker 保持运行，连接恢复后自动查询积压。
    ASSERT_TRUE(gateway.mqtt.start(error)) << error;
    ASSERT_TRUE(inbox->waitMessages(3));
    ASSERT_TRUE(waitUntil([&]() { return rows().empty(); }));
    gateway.publisher.stop();
    checkReceived(3);
    const auto messages = inbox->snapshot();
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[1].payload, offline[0].message);
    EXPECT_EQ(messages[2].payload, offline[1].message);
}

TEST_F(TelemetryPipelineTest, DISABLED_RecreatedGatewayPublishesPersistedBacklog)
{
    std::vector<StoredMessage> backlog;
    {
        Pipeline gateway(db_config, mqtt_config, logger, gateway_id);
        // 不启动 MQTT 客户端，Worker 离线运行，数据必须留在 SQLite。
        ASSERT_TRUE(gateway.publisher.start(error)) << error;
        ASSERT_TRUE(gateway.queue.push(makePacket("persisted-1")));
        ASSERT_TRUE(gateway.queue.push(makePacket("persisted-2")));
        gateway.finishIngestion();
        backlog = rows();
        ASSERT_EQ(backlog.size(), 2U);
    }
    EXPECT_TRUE(inbox->snapshot().empty());
    // 销毁并重建所有网关侧组件，仍使用同一个临时数据库文件。
    Pipeline restarted(db_config, mqtt_config, logger, gateway_id);
    ASSERT_TRUE(restarted.mqtt.start(error)) << error;
    ASSERT_TRUE(restarted.publisher.start(error)) << error;
    ASSERT_TRUE(inbox->waitMessages(2));
    ASSERT_TRUE(waitUntil([&]() { return rows().empty(); }));
    restarted.publisher.stop();
    checkReceived(2);
    const auto messages = inbox->snapshot();
    ASSERT_EQ(messages.size(), backlog.size());
    for (std::size_t i = 0; i < backlog.size(); ++i)
        EXPECT_EQ(messages[i].payload, backlog[i].message);
}
} // namespace
