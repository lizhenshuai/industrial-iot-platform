/*
Linux/POSIX 测试；项目根目录下独立编译，不需要修改 CMakeLists.txt：
g++ -std=c++17 -O0 -g -Wall -Wextra -Wpedantic -pthread \
  -Iinclude -Isrc/infrastructure/mqtt \
  tests/unit/infrastructure/mqtt/MosquittoMqttClientTest.cpp \
  src/infrastructure/mqtt/MosquittoMqttClient.cpp \
  $(pkg-config --cflags --libs libmosquitto yaml-cpp gtest_main) \
  -o /tmp/mosquitto_mqtt_client_tests
本地测试：timeout 120s /tmp/mosquitto_mqtt_client_tests
真实 Broker（显式启用，配置文件可用 MQTT_TEST_CONFIG 指定）：
timeout 45s /tmp/mosquitto_mqtt_client_tests --gtest_also_run_disabled_tests \
  --gtest_filter=MosquittoMqttClientIntegrationTest.DISABLED_ConfiguredBrokerRoundTrip
本文件的服务端仅实现测试需要的 MQTT 3.1.1 报文，不是通用 Broker。

*/

#include "MosquittoMqttClient.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using industrial_gateway::common::ILogger;
using industrial_gateway::common::LogLevel;
using industrial_gateway::config::MqttConfig;
using industrial_gateway::infrastructure::mqtt::MosquittoMqttClient;
using namespace industrial_gateway::application::ports;
using Bytes = std::vector<std::uint8_t>;

class NullLogger final : public ILogger
{
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

// 回调只写入邮箱，所有 GoogleTest 断言都在测试线程执行。
template <class T>
class Mailbox
{
public:
    void push(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.push_back(std::move(value));
        changed_.notify_all();
    }

    std::optional<T> take(std::chrono::milliseconds timeout = 3000ms)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [this] { return !values_.empty(); }))
            return std::nullopt;
        T result = std::move(values_.front());
        values_.pop_front();
        return result;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> values_;
};

// 同步到生产实现已确认连接失败并准备重试，避免把“尚未尝试连接”
// 误当成“离线重试”。日志只用作同步信号，行为仍通过公开接口断言。
class RetryLogger final : public ILogger
{
public:
    void log(LogLevel, std::string_view, std::string_view message) override
    {
        if (message.find("连接请求失败，将在退避后重试") != std::string_view::npos)
            retries.push(true);
    }
    Mailbox<bool> retries;
};

template <class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

void appendU16(Bytes& data, unsigned value)
{
    data.push_back(static_cast<std::uint8_t>(value >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void appendString(Bytes& data, const std::string& value)
{
    appendU16(data, value.size());
    data.insert(data.end(), value.begin(), value.end());
}

unsigned readU16(const Bytes& data, std::size_t offset)
{
    return (static_cast<unsigned>(data.at(offset)) << 8) | data.at(offset + 1);
}

Bytes packet(std::uint8_t header, const Bytes& body)
{
    Bytes result{header};
    auto size = body.size();
    do
    {
        auto byte = static_cast<std::uint8_t>(size % 128);
        size /= 128;
        result.push_back(size ? byte | 0x80 : byte);
    } while (size);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

struct PublishPacket
{
    std::string topic;
    std::string payload;
    unsigned mid;
    int qos;
    bool retained;
};

struct SubscriptionPacket
{
    std::string filter;
    int qos;
};

// 每个实例绑定独立的 loopback 临时端口。只允许一个活动连接，支持重连。
// I/O 有超时；析构先通知退出再 join；工作线程异常转为可检查的错误。
class ScriptedBroker
{
public:
    explicit ScriptedBroker(bool initially_listening = true)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            throw std::runtime_error(std::strerror(errno));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            const std::string error = std::strerror(errno);
            ::close(listener_);
            throw std::runtime_error(error);
        }
        port_ = ntohs(address.sin_port);
        if (initially_listening)
        {
            try { beginListening(); }
            catch (...) { ::close(listener_); throw; }
        }
    }

    ~ScriptedBroker()
    {
        stop();
        ::close(listener_);
    }

    void stop()
    {
        stopping_ = true;
        if (worker_.joinable())
            worker_.join();
        closeConnection();
    }

    ScriptedBroker(const ScriptedBroker&) = delete;
    ScriptedBroker& operator=(const ScriptedBroker&) = delete;

    void beginListening()
    {
        if (::listen(listener_, 4) != 0)
            throw std::runtime_error(std::strerror(errno));
        worker_ = std::thread([this] {
            try { run(); }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = e.what();
            }
        });
    }

    int port() const { return port_; }
    std::string error() const
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_;
    }

    // shutdown 不复用描述符；只有工作线程或 join 后的析构执行 close。
    void disconnect()
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (connection_ >= 0)
            ::shutdown(connection_, SHUT_RDWR);
    }

    bool sendPacket(std::uint8_t header, const Bytes& body)
    {
        const auto bytes = packet(header, body);
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (connection_ < 0)
            return false;
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto count = ::send(connection_, bytes.data() + offset,
                                      bytes.size() - offset, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool acknowledge(unsigned mid)
    {
        Bytes body;
        appendU16(body, mid);
        return sendPacket(0x40, body);
    }

    bool deliver(const std::string& topic, const std::string& payload,
                 int qos = 0, bool retained = false)
    {
        Bytes body;
        appendString(body, topic);
        if (qos)
            appendU16(body, 123);
        body.insert(body.end(), payload.begin(), payload.end());
        return sendPacket(static_cast<std::uint8_t>(0x30 | (qos << 1) | retained), body);
    }

    std::atomic<bool> acknowledge_publishes{true};
    std::atomic<bool> reject_subscriptions{false};
    std::atomic<int> connections{0};
    Mailbox<PublishPacket> publishes;
    Mailbox<SubscriptionPacket> subscriptions;

private:
    void closeConnection()
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (connection_ >= 0)
        {
            ::close(connection_);
            connection_ = -1;
        }
    }

    bool readExact(int fd, std::uint8_t* output, std::size_t size)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        std::size_t received = 0;
        while (received < size && !stopping_)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 25);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                throw std::runtime_error("broker poll failed");
            if (ready == 0)
                continue;
            const auto count = ::recv(fd, output + received, size - received, 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            received += static_cast<std::size_t>(count);
        }
        return received == size;
    }

    bool handle(std::uint8_t header, const Bytes& body)
    {
        switch (header >> 4)
        {
        case 1: // CONNECT
            ++connections;
            return sendPacket(0x20, {0, 0});
        case 3: // PUBLISH：独立解析实际线路报文，验证 QoS/retain/消息内容。
        {
            const auto topic_length = readU16(body, 0);
            std::size_t offset = 2 + topic_length;
            if (offset > body.size())
                throw std::runtime_error("truncated PUBLISH topic");
            PublishPacket message;
            message.topic.assign(body.begin() + 2, body.begin() + offset);
            message.qos = (header >> 1) & 3;
            message.retained = header & 1;
            message.mid = message.qos ? readU16(body, offset) : 0;
            if (message.qos)
                offset += 2;
            message.payload.assign(body.begin() + offset, body.end());
            // 先读取策略再发布事件，避免测试收到事件后更改策略影响当前报文。
            const bool ack = acknowledge_publishes.load();
            publishes.push(message);
            return !ack || message.qos != 1 || acknowledge(message.mid);
        }
        case 8: // SUBSCRIBE（客户端每次仅登记一个 Topic Filter）
        {
            const auto length = readU16(body, 2);
            if (body.size() != 5 + length)
                throw std::runtime_error("unexpected SUBSCRIBE body");
            const int qos = body.back();
            subscriptions.push({std::string(body.begin() + 4, body.end() - 1), qos});
            return sendPacket(0x90, {body.at(0), body.at(1),
                static_cast<std::uint8_t>(reject_subscriptions ? 0x80 : qos)});
        }
        case 12: return sendPacket(0xD0, {}); // PINGREQ
        case 14: return false;               // DISCONNECT
        case 4: return true;                 // 下行 QoS 1 的 PUBACK
        default: throw std::runtime_error("unexpected MQTT packet type");
        }
    }

    void run()
    {
        while (!stopping_)
        {
            pollfd descriptor{listener_, POLLIN, 0};
            if (::poll(&descriptor, 1, 25) <= 0)
                continue;
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0)
                continue;
            timeval send_timeout{1, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                connection_ = fd;
            }
            while (!stopping_)
            {
                std::uint8_t header = 0;
                if (!readExact(fd, &header, 1))
                    break;
                std::size_t remaining = 0;
                std::size_t multiplier = 1;
                bool valid = false;
                bool interrupted = false;
                for (int i = 0; i < 4; ++i)
                {
                    std::uint8_t byte = 0;
                    if (!readExact(fd, &byte, 1))
                    {
                        interrupted = true;
                        break;
                    }
                    remaining += (byte & 127) * multiplier;
                    if (!(byte & 128)) { valid = true; break; }
                    multiplier *= 128;
                }
                // 停止/断线可能发生在报文头和长度之间，属于正常退出。
                if (interrupted)
                    break;
                if (!valid || remaining > 1024 * 1024)
                    throw std::runtime_error("invalid/oversized MQTT packet");
                Bytes body(remaining);
                if (!readExact(fd, body.data(), body.size()) || !handle(header, body))
                    break;
            }
            closeConnection();
        }
    }

    int listener_ = -1;
    int connection_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex socket_mutex_;
    mutable std::mutex error_mutex_;
    std::string error_;
};

MqttConfig localConfig(int port)
{
    static std::atomic<unsigned> sequence{0};
    MqttConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.client_id = "mqtt-test-" + std::to_string(::getpid()) + "-" +
                       std::to_string(++sequence);
    return config;
}

// 即使 ASSERT 提前退出，也先 stop 唤醒 publish，再回收异步任务。
class PendingPublish
{
public:
    explicit PendingPublish(MosquittoMqttClient& client) : client_(client),
        result(std::async(std::launch::async, [&client] {
            return client.publish("test/pending", "payload", 10s);
        })) {}
    ~PendingPublish()
    {
        if (result.valid())
        {
            client_.stop();
            result.wait();
        }
    }
private:
    MosquittoMqttClient& client_;
public:
    std::future<MqttPublishResult> result;
};

class MosquittoMqttClientTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        broker = std::make_unique<ScriptedBroker>();
        config = localConfig(broker->port());
        client = std::make_unique<MosquittoMqttClient>(config, logger);
    }
    void TearDown() override
    {
        client.reset();
        if (broker)
        {
            broker->stop();
            EXPECT_TRUE(broker->error().empty()) << broker->error();
        }
        broker.reset();
    }
    bool connect()
    {
        return client->start(error) && waitUntil([this] { return client->isConnected(); });
    }
    NullLogger logger;
    std::unique_ptr<ScriptedBroker> broker;
    MqttConfig config;
    std::unique_ptr<MosquittoMqttClient> client;
    std::string error;
};

TEST_F(MosquittoMqttClientTest, InitialStateAndStoppedPublish)
{
    EXPECT_FALSE(client->isConnected());
    EXPECT_EQ(client->publish("test/a", "x", 1s).status, MqttPublishStatus::NotConnected);
    client->stop();
    client->stop();
    EXPECT_FALSE(client->isConnected());
}

TEST_F(MosquittoMqttClientTest, DisabledClientDoesNotConnectOrSubscribe)
{
    config.enabled = false;
    client = std::make_unique<MosquittoMqttClient>(config, logger);
    error = "stale error";
    EXPECT_TRUE(client->start(error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(client->isConnected());
    EXPECT_FALSE(client->subscribe("test/#", 1, [](const auto&) {}, {}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(client->publish("test/a", "x", 1s).status, MqttPublishStatus::NotConnected);
    EXPECT_EQ(broker->connections.load(), 0);
}

TEST_F(MosquittoMqttClientTest, RepeatedStartStopAndRestart)
{
    ASSERT_TRUE(connect()) << error;
    error = "stale error";
    EXPECT_TRUE(client->start(error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(broker->connections.load(), 1);
    client->stop();
    client->stop();
    EXPECT_FALSE(client->isConnected());
    EXPECT_EQ(client->publish("test/a", "x", 1s).status, MqttPublishStatus::NotConnected);
    ASSERT_TRUE(connect()) << error;
    EXPECT_EQ(broker->connections.load(), 2);
    EXPECT_TRUE(client->publish("test/a", "restart", 2s).isAcknowledged());
}

TEST_F(MosquittoMqttClientTest, ConfigurationIsCopiedAtConstruction)
{
    config.host.clear();
    config.client_id.clear();
    config.port = 0;
    EXPECT_TRUE(connect()) << error;
}

TEST_F(MosquittoMqttClientTest, InvalidConfigurationIsRejected)
{
    for (int variant = 0; variant < 9; ++variant)
    {
        SCOPED_TRACE(variant);
        auto invalid = config;
        switch (variant)
        {
        case 0: invalid.host.clear(); break;
        case 1: invalid.client_id.clear(); break;
        case 2: invalid.port = 0; break;
        case 3: invalid.port = -1; break;
        case 4: invalid.port = 65536; break;
        case 5: invalid.keep_alive = -1; break;
        case 6: invalid.keep_alive = 1; break;
        case 7: invalid.keep_alive = 4; break;
        case 8: invalid.keep_alive = 3601; break;
        }
        MosquittoMqttClient invalid_client(invalid, logger);
        EXPECT_FALSE(invalid_client.start(error));
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(invalid_client.isConnected());
    }
}

TEST_F(MosquittoMqttClientTest, KeepAliveBoundariesAreAccepted)
{
    for (int keep_alive : {0, 5, 3600})
    {
        SCOPED_TRACE(keep_alive);
        client.reset();
        config.keep_alive = keep_alive;
        client = std::make_unique<MosquittoMqttClient>(config, logger);
        ASSERT_TRUE(connect()) << error;
        client->stop();
    }
}

TEST_F(MosquittoMqttClientTest, InvalidPublishArgumentsAreRejected)
{
    ASSERT_TRUE(connect()) << error;
    for (const auto& topic : std::vector<std::string>{"", "test/+", "test/#", std::string("a\0b", 3)})
    {
        auto result = client->publish(topic, "x", 1s);
        EXPECT_EQ(result.status, MqttPublishStatus::PublishFailed);
        EXPECT_FALSE(result.error_message.empty());
    }
    for (auto timeout : {0ms, -1ms})
        EXPECT_EQ(client->publish("test/a", "x", timeout).status, MqttPublishStatus::InternalError);
    EXPECT_FALSE(broker->publishes.take(50ms).has_value());
}

TEST_F(MosquittoMqttClientTest, PublishUsesQosOneWithoutRetainAndPreservesPayload)
{
    ASSERT_TRUE(connect()) << error;
    for (const auto& payload : std::vector<std::string>{"", "hello", std::string("a\0b", 3), std::string(1024, 'x')})
    {
        auto result = client->publish("test/telemetry", payload, 2s);
        ASSERT_EQ(result.status, MqttPublishStatus::Acknowledged) << result.error_message;
        EXPECT_TRUE(result.error_message.empty());
        auto received = broker->publishes.take();
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->topic, "test/telemetry");
        EXPECT_EQ(received->payload, payload);
        EXPECT_EQ(received->qos, 1);
        EXPECT_FALSE(received->retained);
        EXPECT_NE(received->mid, 0u);
    }
}

TEST_F(MosquittoMqttClientTest, OnlyMatchingPubackCompletesPublish)
{
    broker->acknowledge_publishes = false;
    ASSERT_TRUE(connect()) << error;
    PendingPublish pending(*client);
    auto message = broker->publishes.take();
    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(broker->acknowledge(message->mid == 65535 ? 1 : message->mid + 1));
    EXPECT_EQ(pending.result.wait_for(100ms), std::future_status::timeout);
    ASSERT_TRUE(broker->acknowledge(message->mid));
    ASSERT_EQ(pending.result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(pending.result.get().status, MqttPublishStatus::Acknowledged);
}

TEST_F(MosquittoMqttClientTest, TimeoutAndLatePubackDoNotCompleteNextPublish)
{
    broker->acknowledge_publishes = false;
    ASSERT_TRUE(connect()) << error;
    const auto began = std::chrono::steady_clock::now();
    auto result = client->publish("test/first", "first", 150ms);
    EXPECT_EQ(result.status, MqttPublishStatus::AcknowledgementTimeout);
    EXPECT_GE(std::chrono::steady_clock::now() - began, 150ms);
    auto first = broker->publishes.take();
    ASSERT_TRUE(first.has_value());
    PendingPublish second(*client);
    auto next = broker->publishes.take();
    ASSERT_TRUE(next.has_value());
    ASSERT_NE(first->mid, next->mid);
    ASSERT_TRUE(broker->acknowledge(first->mid));
    EXPECT_EQ(second.result.wait_for(100ms), std::future_status::timeout);
    ASSERT_TRUE(broker->acknowledge(next->mid));
    ASSERT_EQ(second.result.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(second.result.get().isAcknowledged());
}

TEST_F(MosquittoMqttClientTest, StopWakesPendingPublish)
{
    broker->acknowledge_publishes = false;
    ASSERT_TRUE(connect()) << error;
    PendingPublish pending(*client);
    ASSERT_TRUE(broker->publishes.take().has_value());
    const auto began = std::chrono::steady_clock::now();
    client->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - began, 3s);
    ASSERT_EQ(pending.result.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(pending.result.get().status, MqttPublishStatus::InternalError);
    EXPECT_FALSE(client->isConnected());
}

TEST_F(MosquittoMqttClientTest, DisconnectWakesPendingPublishAndReconnects)
{
    broker->acknowledge_publishes = false;
    ASSERT_TRUE(connect()) << error;
    PendingPublish pending(*client);
    ASSERT_TRUE(broker->publishes.take().has_value());
    broker->disconnect();
    ASSERT_EQ(pending.result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(pending.result.get().status, MqttPublishStatus::ConnectionLost);
    ASSERT_TRUE(waitUntil([this] { return broker->connections >= 2 && client->isConnected(); }, 5s));
    broker->acknowledge_publishes = true;
    EXPECT_TRUE(client->publish("test/recovered", "ok", 2s).isAcknowledged());
}

TEST_F(MosquittoMqttClientTest, InvalidSubscriptionsAreRejected)
{
    for (const auto& topic : std::vector<std::string>{"", "test/#/bad", "test/a+", std::string("a\0b", 3)})
    {
        EXPECT_FALSE(client->subscribe(topic, 1, [](const auto&) {}, {}, error));
        EXPECT_FALSE(error.empty());
    }
    for (int qos : {-1, 3})
        EXPECT_FALSE(client->subscribe("test/#", qos, [](const auto&) {}, {}, error));
    EXPECT_FALSE(client->subscribe("test/#", 1, {}, {}, error));
    error = "stale error";
    EXPECT_TRUE(client->subscribe("test/#", 1, [](const auto&) {}, {}, error));
    EXPECT_TRUE(error.empty());
}

TEST_F(MosquittoMqttClientTest, PreRegisteredSubscriptionsReceiveGrantedQos)
{
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    for (int qos : {0, 1, 2})
        ASSERT_TRUE(client->subscribe("test/qos/" + std::to_string(qos), qos,
            [](const auto&) {}, [results](const auto& result) { results->push(result); }, error));
    ASSERT_TRUE(connect()) << error;
    bool seen[3] = {false, false, false};
    for (int i = 0; i < 3; ++i)
    {
        auto result = results->take();
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->status, MqttSubscriptionStatus::Acknowledged);
        ASSERT_GE(result->granted_qos, 0);
        ASSERT_LE(result->granted_qos, 2);
        seen[result->granted_qos] = true;
        auto request = broker->subscriptions.take();
        ASSERT_TRUE(request.has_value());
        EXPECT_EQ(request->filter, "test/qos/" + std::to_string(request->qos));
    }
    EXPECT_TRUE(seen[0] && seen[1] && seen[2]);
}

TEST_F(MosquittoMqttClientTest, RejectedSubscriptionReportsFailure)
{
    broker->reject_subscriptions = true;
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    ASSERT_TRUE(connect()) << error;
    ASSERT_TRUE(client->subscribe("test/denied", 1, [](const auto&) {},
        [results](const auto& result) { results->push(result); }, error));
    auto result = results->take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, MqttSubscriptionStatus::Rejected);
    EXPECT_EQ(result->granted_qos, -1);
    EXPECT_FALSE(result->error_message.empty());
    EXPECT_TRUE(client->isConnected());
}

TEST_F(MosquittoMqttClientTest, WildcardsDispatchBinaryMessagesAndMetadata)
{
    auto plus = std::make_shared<Mailbox<MqttMessage>>();
    auto hash = std::make_shared<Mailbox<MqttMessage>>();
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    auto on_result = [results](const auto& r) { results->push(r); };
    ASSERT_TRUE(client->subscribe("test/+/value", 1,
        [plus](const auto& m) { plus->push(m); }, on_result, error));
    ASSERT_TRUE(client->subscribe("test/#", 1,
        [hash](const auto& m) { hash->push(m); }, on_result, error));
    ASSERT_TRUE(connect()) << error;
    ASSERT_TRUE(results->take().has_value());
    ASSERT_TRUE(results->take().has_value());
    const std::string payload("a\0b", 3);
    ASSERT_TRUE(broker->deliver("test/device/value", payload, 1, true));
    for (const auto& inbox : {plus, hash})
    {
        auto message = inbox->take();
        ASSERT_TRUE(message.has_value());
        EXPECT_EQ(message->topic, "test/device/value");
        EXPECT_EQ(message->payload, payload);
        EXPECT_EQ(message->qos, 1);
        EXPECT_TRUE(message->retained);
    }
    ASSERT_TRUE(broker->deliver("test/device/deeper/value", ""));
    auto deep = hash->take();
    ASSERT_TRUE(deep.has_value());
    EXPECT_TRUE(deep->payload.empty());
    EXPECT_FALSE(plus->take(100ms).has_value());
}

TEST_F(MosquittoMqttClientTest, DuplicateSubscriptionUsesLatestCallbackAndQos)
{
    auto old_messages = std::make_shared<Mailbox<MqttMessage>>();
    auto new_messages = std::make_shared<Mailbox<MqttMessage>>();
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    ASSERT_TRUE(client->subscribe("test/same", 0,
        [old_messages](const auto& m) { old_messages->push(m); }, {}, error));
    ASSERT_TRUE(client->subscribe("test/same", 1,
        [new_messages](const auto& m) { new_messages->push(m); },
        [results](const auto& r) { results->push(r); }, error));
    ASSERT_TRUE(connect()) << error;
    auto result = results->take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->granted_qos, 1);
    ASSERT_TRUE(broker->deliver("test/same", "new"));
    ASSERT_TRUE(new_messages->take().has_value());
    EXPECT_FALSE(old_messages->take(100ms).has_value());
}

TEST_F(MosquittoMqttClientTest, ReconnectRestoresSubscriptions)
{
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    auto messages = std::make_shared<Mailbox<MqttMessage>>();
    ASSERT_TRUE(client->subscribe("test/restored", 1,
        [messages](const auto& m) { messages->push(m); },
        [results](const auto& r) { results->push(r); }, error));
    ASSERT_TRUE(connect()) << error;
    ASSERT_TRUE(results->take().has_value());
    ASSERT_TRUE(broker->subscriptions.take().has_value());
    broker->disconnect();
    auto restored = results->take(5s);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->isAcknowledged());
    auto subscription = broker->subscriptions.take();
    ASSERT_TRUE(subscription.has_value());
    EXPECT_EQ(subscription->filter, "test/restored");
    EXPECT_EQ(subscription->qos, 1);
    ASSERT_TRUE(broker->deliver("test/restored", "after reconnect"));
    auto message = messages->take();
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->payload, "after reconnect");
}

TEST_F(MosquittoMqttClientTest, CallbackExceptionsDoNotStopNetworkLoop)
{
    auto results = std::make_shared<Mailbox<int>>();
    auto messages = std::make_shared<Mailbox<int>>();
    ASSERT_TRUE(client->subscribe("test/throw", 1,
        [messages](const auto&) { messages->push(1); throw std::runtime_error("message exception"); },
        [results](const auto&) { results->push(1); throw std::runtime_error("result exception"); }, error));
    ASSERT_TRUE(connect()) << error;
    ASSERT_TRUE(results->take().has_value());
    for (int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(broker->deliver("test/throw", "x"));
        ASSERT_TRUE(messages->take().has_value());
        EXPECT_TRUE(client->publish("test/alive", "x", 2s).isAcknowledged());
    }
}

TEST_F(MosquittoMqttClientTest, PublishInCallbackIsRejectedWithoutDeadlock)
{
    auto results = std::make_shared<Mailbox<MqttPublishResult>>();
    auto* mqtt = client.get();
    ASSERT_TRUE(client->subscribe("test/callback", 1, [](const auto&) {},
        [results, mqtt](const auto&) { results->push(mqtt->publish("test/a", "x", 1s)); }, error));
    ASSERT_TRUE(connect()) << error;
    auto result = results->take();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, MqttPublishStatus::InternalError);
    EXPECT_TRUE(client->publish("test/alive", "x", 2s).isAcknowledged());
}

TEST_F(MosquittoMqttClientTest, SubscribeInsideCallbackDoesNotHoldSubscriptionLock)
{
    auto registered = std::make_shared<Mailbox<bool>>();
    auto results = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    auto* mqtt = client.get();
    ASSERT_TRUE(client->subscribe("test/first", 1, [](const auto&) {},
        [registered, results, mqtt](const auto&) {
            std::string message;
            registered->push(mqtt->subscribe("test/second", 1, [](const auto&) {},
                [results](const auto& r) { results->push(r); }, message));
        }, error));
    ASSERT_TRUE(connect()) << error;
    auto success = registered->take();
    ASSERT_TRUE(success.has_value());
    EXPECT_TRUE(*success);
    auto result = results->take();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->isAcknowledged());
}

TEST_F(MosquittoMqttClientTest, DestroyingOneClientDoesNotBreakAnother)
{
    ScriptedBroker second_broker;
    MosquittoMqttClient second(localConfig(second_broker.port()), logger);
    ASSERT_TRUE(connect()) << error;
    ASSERT_TRUE(second.start(error));
    ASSERT_TRUE(waitUntil([&] { return second.isConnected(); }));
    client.reset(); // 析构负责停止并释放该实例资源。
    EXPECT_TRUE(second.publish("test/still-alive", "ok", 2s).isAcknowledged());
    second.stop();
    EXPECT_TRUE(second_broker.error().empty()) << second_broker.error();
}

TEST(MosquittoMqttClientOfflineTest, UnavailableBrokerCanStartAndConnectLater)
{
    RetryLogger logger;
    ScriptedBroker broker(false); // 已占用端口，但尚未监听，不存在端口抢占窗口。
    MosquittoMqttClient client(localConfig(broker.port()), logger);
    std::string error;
    ASSERT_TRUE(client.start(error)) << error;
    EXPECT_FALSE(client.isConnected());
    EXPECT_EQ(client.publish("test/offline", "x", 1s).status, MqttPublishStatus::NotConnected);
    ASSERT_TRUE(logger.retries.take().has_value()) << "Client did not enter connection retry";
    broker.beginListening();
    ASSERT_TRUE(waitUntil([&] { return client.isConnected(); }, 5s));
    EXPECT_TRUE(client.publish("test/online", "ok", 2s).isAcknowledged());
    client.stop();
    EXPECT_TRUE(broker.error().empty()) << broker.error();
}

TEST(MosquittoMqttClientOfflineTest, StopDuringConnectionRetryReturnsPromptly)
{
    RetryLogger logger;
    ScriptedBroker broker(false);
    MosquittoMqttClient client(localConfig(broker.port()), logger);
    std::string error;
    ASSERT_TRUE(client.start(error));
    ASSERT_TRUE(logger.retries.take().has_value()) << "Client did not enter connection retry";
    const auto began = std::chrono::steady_clock::now();
    client.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
    EXPECT_FALSE(client.isConnected());
}

// 默认禁用，避免普通单元测试依赖局域网。显式运行时连接失败必须报错。
TEST(MosquittoMqttClientIntegrationTest, DISABLED_ConfiguredBrokerRoundTrip)
{
    const char* override_path = std::getenv("MQTT_TEST_CONFIG");
    const std::filesystem::path path = override_path ? override_path : "config/gateway.yaml";
    const auto root = YAML::LoadFile(path.string());
    const auto mqtt = root["mqtt"];
    ASSERT_TRUE(mqtt.IsMap()) << path;
    MqttConfig config;
    config.enabled = mqtt["enabled"].as<bool>();
    config.host = mqtt["host"].as<std::string>();
    config.port = mqtt["port"].as<int>();
    config.client_id = mqtt["client_id"].as<std::string>();
    config.keep_alive = mqtt["keep_alive"].as<int>();
    ASSERT_TRUE(config.enabled) << "MQTT is disabled in " << path;
    const auto suffix = std::to_string(::getpid()) + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    config.client_id += "-test-" + suffix;
    const auto topic = "gateway-tests/" + suffix + "/roundtrip";
    const std::string payload("mqtt-test\0binary", 16);
    auto subscriptions = std::make_shared<Mailbox<MqttSubscriptionResult>>();
    auto messages = std::make_shared<Mailbox<MqttMessage>>();
    NullLogger logger;
    MosquittoMqttClient client(config, logger);
    std::string error;
    ASSERT_TRUE(client.subscribe(topic, 1,
        [messages](const auto& message) { messages->push(message); },
        [subscriptions](const auto& result) { subscriptions->push(result); }, error)) << error;
    ASSERT_TRUE(client.start(error)) << error;
    ASSERT_TRUE(waitUntil([&] { return client.isConnected(); }, 10s))
        << "Cannot connect to " << config.host << ':' << config.port;
    auto subscription = subscriptions->take(5s);
    ASSERT_TRUE(subscription.has_value());
    ASSERT_TRUE(subscription->isAcknowledged()) << subscription->error_message;
    auto published = client.publish(topic, payload, 5s);
    ASSERT_TRUE(published.isAcknowledged()) << published.error_message;
    auto received = messages->take(5s);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->topic, topic);
    EXPECT_EQ(received->payload, payload);
    EXPECT_EQ(received->qos, 1);
    EXPECT_FALSE(received->retained);
    client.stop();
    EXPECT_FALSE(client.isConnected());
}

} // namespace
