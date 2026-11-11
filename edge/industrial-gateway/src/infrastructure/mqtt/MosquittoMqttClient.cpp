
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include "MosquittoMqttClient.hpp"
#include "industrial_gateway/common/RuntimeFailure.hpp"

#include "industrial_gateway/common/LogLevel.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


namespace industrial_gateway::infrastructure::mqtt
{

namespace
{

// ==================================================
// MQTT 客户端日志组件名称
// ==================================================

constexpr std::string_view kLogComponent =
    "MosquittoMqttClient";


// ==================================================
// MQTT 重连策略
// ==================================================

constexpr std::chrono::milliseconds kInitialReconnectDelay{
    1000
};

constexpr std::chrono::milliseconds kMaxReconnectDelay{
    30000
};


// ==================================================
// MQTT 网络循环等待时间
// ==================================================

constexpr int kNetworkLoopTimeoutMs = 100;

constexpr int kNetworkLoopMaxPackets = 1;

// 包含调用方已经超时、但 libmosquitto 仍在等待 PUBACK 的消息。
constexpr std::size_t kMaxOutstandingPublishes = 20;

// 优雅断开连接的最长等待时间。
constexpr std::chrono::milliseconds kGracefulDisconnectTimeout{
    2000
};


// ==================================================
// 判断 Mosquitto 网络错误是否属于不可恢复错误
// ==================================================
//
// 分类参考 libmosquitto 2.0.x 的 mosquitto_loop_forever()：
// 协议错误、参数错误、TLS/认证错误、内存错误等不应无限重连。
// 普通断线、无连接和大多数系统网络错误仍允许重试。
//
// MOSQ_ERR_ERRNO 需要进一步检查 errno：EPROTO 表示协议错误，
// 其余常见网络错误（例如 ECONNREFUSED）允许退避重试。
//
// ==================================================

bool isFatalMosquittoError(
    int result_code,
    int system_error = errno
) noexcept
{
    switch (result_code)
    {
        case MOSQ_ERR_NOMEM:
        case MOSQ_ERR_PROTOCOL:
        case MOSQ_ERR_INVAL:
        case MOSQ_ERR_NOT_FOUND:
        case MOSQ_ERR_TLS:
        case MOSQ_ERR_PAYLOAD_SIZE:
        case MOSQ_ERR_NOT_SUPPORTED:
        case MOSQ_ERR_AUTH:
        case MOSQ_ERR_ACL_DENIED:
        case MOSQ_ERR_UNKNOWN:
        case MOSQ_ERR_PROXY:
            return true;

        case MOSQ_ERR_EAI:
            // libmosquitto 将 getaddrinfo 的错误码保存在连接线程的 errno 中。
            return system_error != EAI_AGAIN;

        case MOSQ_ERR_ERRNO:
            return system_error == EPROTO;

        default:
            return false;
    }
}


// MQTT 3.1.1 CONNACK：Server Unavailable。
// 当前客户端未显式切换 MQTT 5，因此按 libmosquitto 默认 MQTT 3.1.1
// 的 CONNACK 返回码处理。其他拒绝原因视为配置/权限类永久错误。
constexpr int kConnackServerUnavailable = 3;


// ==================================================
// 当前 MQTT 网络线程
// ==================================================
//
// 用于防止在 MQTT 回调线程中调用阻塞式 publish()，
// 避免等待 PUBACK 时发生死锁。
//
// ==================================================

thread_local const void* gCurrentMqttNetworkInstance = nullptr;


// ==================================================
// Mosquitto 全局库生命周期管理
// ==================================================
//
// Mosquitto 的初始化和清理属于进程级资源。
//
// 多个客户端实例共享同一个 Mosquitto 全局库。
//
// 使用引用计数，避免某个客户端提前清理全局库，
// 影响其他仍在运行的 MQTT 客户端。
//
// ==================================================

class MosquittoLibraryManager final
{
public:

    static std::shared_ptr<MosquittoLibraryManager> instance()
    {
        static const auto manager = std::shared_ptr<MosquittoLibraryManager>(
            new MosquittoLibraryManager()
        );
        return manager;
    }

    // ==================================================
    // 初始化并获取库引用
    // ==================================================

    bool acquire(
        std::string& error_message
    )
    {
        error_message.clear();

        std::lock_guard<std::mutex> lock(mutex_);

        if (reference_count_ == 0)
        {
            const int rc = mosquitto_lib_init();

            if (rc != MOSQ_ERR_SUCCESS)
            {
                error_message =
                    "Mosquitto 全局库初始化失败: " +
                    std::to_string(rc);

                return false;
            }

        }

        ++reference_count_;

        return true;
    }


    // ==================================================
    // 释放库引用
    // ==================================================

    void release() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (reference_count_ == 0)
        {
            return;
        }

        if (--reference_count_ == 0)
        {
            mosquitto_lib_cleanup();
        }
    }


private:

    // ==================================================
    // 全局状态
    // ==================================================

    MosquittoLibraryManager() = default;
    std::mutex mutex_;
    std::size_t reference_count_ = 0;

};

} // namespace


// ==================================================
// MosquittoMqttClient::Impl
// ==================================================

class MosquittoMqttClient::Impl final
{
public:

    // ==================================================
    // 构造函数
    // ==================================================

    Impl(
        const ::industrial_gateway::config::MqttConfig& config,
        ::industrial_gateway::common::ILogger& logger
    )
        : config_(config),
          logger_(logger)
    {
    }


    // ==================================================
    // 析构函数
    // ==================================================

    ~Impl()
    {
        stop();
    }


    // ==================================================
    // 禁止复制
    // ==================================================

    Impl(const Impl&) = delete;

    Impl& operator=(const Impl&) = delete;


    // ==================================================
    // MQTT 客户端启动
    // ==================================================

    bool start(
        std::string& error_message
    )
    {
        error_message.clear();

        // 外部 stop() 可能正持有生命周期锁，等待当前回调返回。
        if (isCurrentNetworkThread())
        {
            if (stopping_.load())
            {
                error_message = "MQTT 网络线程正在停止";
                return false;
            }
            return true;
        }

        std::lock_guard<std::mutex> lifecycle_lock(
            lifecycle_mutex_
        );

        // 已经启动。
        // 如果网络线程已经请求停止或发生不可恢复异常，
        // 不能把“线程已经退出”的客户端误报为启动成功。
        if (started_.load())
        {
            if (stopping_.load() || network_failed_.load())
            {
                error_message =
                    "MQTT 客户端已停止或网络线程异常退出，"
                    "请先调用 stop() 完成资源回收，再重新启动";

                return false;
            }

            return true;
        }

        // MQTT 被配置为禁用
        if (!config_.enabled)
        {
            log(
                ::industrial_gateway::common::LogLevel::Info,
                "MQTT 已通过配置禁用"
            );

            return true;
        }


        // ==================================================
        // 1. 校验基本配置
        // ==================================================

        if (
            config_.host.empty() ||
            config_.client_id.empty() ||
            config_.port < 1 ||
            config_.port > 65535 ||
            (config_.keep_alive != 0 && config_.keep_alive < 5) ||
            config_.keep_alive > 3600
        )
        {
            error_message =
                "MQTT 配置不合法：host/client_id 不能为空，port 必须在 1..65535，keep_alive 必须为 0 或 5..3600";

            return false;
        }


        // ==================================================
        // 2. 初始化 Mosquitto 全局库
        // ==================================================

        if (
            !library_->acquire(
                error_message
            )
        )
        {
            return false;
        }

        // ==================================================
        // 3. 创建 Mosquitto 客户端
        // ==================================================

        try
        {
            mosquitto_.reset(
                mosquitto_new(config_.client_id.c_str(), true, nullptr),
                [library = library_](struct mosquitto* client) noexcept
                {
                    if (client != nullptr)
                    {
                        mosquitto_destroy(client);
                    }
                    library->release();
                }
            );
        }
        catch (const std::bad_alloc&)
        {
            // shared_ptr 建立失败时也会调用上面的删除器。
            error_message = "创建 Mosquitto 客户端失败：内存不足";
            return false;
        }

        if (mosquitto_ == nullptr)
        {
            error_message =
                "创建 Mosquitto 客户端失败";

            mosquitto_.reset();

            return false;
        }


        // ==================================================
        // 4. 设置 MQTT 回调
        // ==================================================

        mosquitto_connect_callback_set(
            mosquitto_.get(),
            &Impl::onConnect
        );

        mosquitto_disconnect_callback_set(
            mosquitto_.get(),
            &Impl::onDisconnect
        );

        mosquitto_publish_callback_set(
            mosquitto_.get(),
            &Impl::onPublish
        );

        mosquitto_subscribe_callback_set(
            mosquitto_.get(),
            &Impl::onSubscribe
        );

        mosquitto_message_callback_set(
            mosquitto_.get(),
            &Impl::onMessage
        );


        // ==================================================
        // 5. 启用多线程支持
        // ==================================================
        //
        // 网络循环由独立线程执行。
        //
        // publish() 由 MqttPublisher 线程调用。
        //
        // 因此需要明确告知 Mosquitto 当前客户端
        // 存在多个线程并发访问。
        //
        // ==================================================

        int rc = mosquitto_threaded_set(
            mosquitto_.get(),
            true
        );

        if (rc != MOSQ_ERR_SUCCESS)
        {
            error_message =
                "启用 Mosquitto 多线程支持失败: " +
                std::string(mosquitto_strerror(rc));

            mosquitto_.reset();

            return false;
        }


        // ==================================================
        // 6. 初始化运行状态
        // ==================================================
        //
        // 网络线程安排同步连接；连接完成后由该线程独占 mosquitto_loop()。
        //
        // ==================================================


        failure_.reset();
        stopping_.store(false);

        connected_.store(false);

        reconnect_requested_.store(false);

        network_failed_.store(false);

        pending_subscriptions_.clear();

        disconnect_completed_.store(false);


        // ==================================================
        // 7. 启动网络线程
        // ==================================================

        try
        {
            network_running_.store(true);
            network_thread_ = std::thread([this]()
            {
                networkLoop();
                if (!stopping_.load())
                    failure_.record("MQTT 网络线程未经停止请求退出");
                network_running_.store(false);
            });
            started_.store(true);
        }
        catch (const std::exception& e)
        {
            network_running_.store(false);
            started_.store(false);

            stopping_.store(true);

            error_message =
                "启动 MQTT 网络线程失败: " +
                std::string(e.what());

            mosquitto_.reset();

            // 线程没有成功创建，资源已经全部回收，
            // 允许后续重新注册订阅并再次 start()。
            stopping_.store(false);
            network_failed_.store(false);

            return false;
        }


        log(
            ::industrial_gateway::common::LogLevel::Info,
            "MQTT 客户端启动成功，正在连接 Broker"
        );

        return true;
    }


    // ==================================================
    // MQTT 客户端停止
    // ==================================================

    void stop() noexcept
    {
        // 必须在获取 lifecycle_mutex_ 之前判断当前线程。
        // 否则可能出现：外部线程持有 lifecycle_mutex_ 并 join 网络线程，
        // 网络线程中的用户回调又调用 stop() 并等待同一把锁，最终死锁。
        if (gCurrentMqttNetworkInstance == this)
        {
            requestStop();

            connected_.store(false);

            failPendingPublishes(
                ::industrial_gateway::application::ports::
                    MqttPublishStatus::InternalError,
                "不能在 MQTT 网络回调线程中执行完整 stop()"
            );

            log(
                ::industrial_gateway::common::LogLevel::Error,
                "检测到 MQTT 网络线程调用 stop()，"
                "已请求停止；网络线程将执行断开，仍需外部管理线程完成资源回收"
            );

            return;
        }


        std::lock_guard<std::mutex> lifecycle_lock(
            lifecycle_mutex_
        );


        // ==================================================
        // 1. 检查是否已经停止
        // ==================================================

        if (
            !started_.load() &&
            mosquitto_ == nullptr
        )
        {
            return;
        }


        // ==================================================
        // 2. 设置停止标志
        // ==================================================

        requestStop();

        connected_.store(false);


        // ==================================================
        // 3. 唤醒所有等待 PUBACK 的线程
        // ==================================================

        failPendingPublishes(
            ::industrial_gateway::application::ports::
                MqttPublishStatus::InternalError,
            "MQTT 客户端正在停止"
        );


        // ==================================================
        // 4. 清理待处理订阅命令
        // ==================================================

        {
            std::lock_guard<std::mutex> lock(
                subscription_mutex_
            );

            subscription_queue_.clear();

            queued_subscriptions_.clear();
        }


        // ==================================================
        // 5. 等待网络线程退出。停止信号也会唤醒它对连接结果的等待。
        // ==================================================

        if (network_thread_.joinable())
        {
            try
            {
                network_thread_.join();
            }
            catch (const std::exception& e)
            {
                log(
                    ::industrial_gateway::common::LogLevel::Critical,
                    "等待 MQTT 网络线程退出失败"
                );

                log(
                    ::industrial_gateway::common::LogLevel::Critical,
                    e.what()
                );

                // 不能在线程仍然运行时释放 Impl 和 Mosquitto 资源，
                // 否则可能产生悬空访问。
                std::terminate();
            }
            catch (...)
            {
                log(
                    ::industrial_gateway::common::LogLevel::Critical,
                    "等待 MQTT 网络线程退出时发生未知异常"
                );

                std::terminate();
            }
        }


        // ==================================================
        // 6. 等待正在执行的 publish() 完全退出
        // ==================================================

        {
            std::unique_lock<std::mutex> lock(
                active_publish_mutex_
            );

            active_publish_cv_.wait(
                lock,
                [this]()
                {
                    return active_publish_calls_ == 0;
                }
            );
            started_.store(false);
        }


        // ==================================================
        // 7. 释放 Mosquitto 资源
        // ==================================================

        mosquitto_.reset();
        pending_publishes_.fill(nullptr);


        // ==================================================
        // 8. 更新运行状态
        // ==================================================

        reconnect_requested_.store(false);

        network_failed_.store(false);

        // 网络线程已经 join。尚未返回的系统连接只持有旧句柄。
        // 这样允许在下一次 start() 之前重新登记订阅意图。
        stopping_.store(false);

        log(
            ::industrial_gateway::common::LogLevel::Info,
            "MQTT 客户端已停止"
        );
    }

    // ==================================================
    // 查询连接状态
    // ==================================================

    bool isConnected() const noexcept
    {
        return connected_.load();
    }

    bool isRunning() const noexcept
    {
        return network_running_.load() && !failure_.hasFailure();
    }

    std::string failureReason() const { return failure_.reason(); }


    // 判断当前线程是否为本实例的 MQTT 网络线程。
    // 在析构函数中用于避免在网络回调线程销毁自身运行所依赖的 Impl。
    bool isCurrentNetworkThread() const noexcept
    {
        return gCurrentMqttNetworkInstance == this;
    }


    // ==================================================
    // MQTT 消息发布
    // ==================================================

    ::industrial_gateway::application::ports::
        MqttPublishResult publish(
            const std::string& topic,
            const std::string& payload,
            std::chrono::milliseconds acknowledgement_timeout
        )
    {
        using namespace ::industrial_gateway::application::ports;


        // ==================================================
        // 1. 检查是否从 MQTT 网络线程调用
        // ==================================================

        if (gCurrentMqttNetworkInstance == this)
        {
            return makePublishResult(
                MqttPublishStatus::InternalError,
                "不允许在 MQTT 网络回调线程中调用 publish()"
            );
        }


        // ==================================================
        // 2. 注册发布操作，在 Topic 校验和发布期间保持库与客户端有效
        // ==================================================

        if (!beginActivePublish())
        {
            return makePublishResult(
                MqttPublishStatus::NotConnected,
                "MQTT 客户端尚未启动或正在停止"
            );
        }

        ActivePublishGuard active_guard(*this);

        // 3. 检查参数
        if (
            topic.empty() ||
            topic.find('\0') != std::string::npos ||
            mosquitto_pub_topic_check(topic.c_str()) !=
                MOSQ_ERR_SUCCESS
        )
        {
            return makePublishResult(
                MqttPublishStatus::PublishFailed,
                "MQTT 发布 Topic 不合法"
            );
        }

        if (
            payload.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )
        )
        {
            return makePublishResult(
                MqttPublishStatus::PublishFailed,
                "MQTT 消息长度超过 Mosquitto 支持范围"
            );
        }

        if (acknowledgement_timeout.count() <= 0)
        {
            return makePublishResult(
                MqttPublishStatus::InternalError,
                "PUBACK 等待超时时间必须大于 0"
            );
        }


        // ==================================================
        // 4. 创建当前消息的等待对象
        // ==================================================

        std::shared_ptr<PendingPublish> pending;

        try
        {
            pending = std::make_shared<PendingPublish>();
        }
        catch (const std::exception& e)
        {
            return makePublishResult(
                MqttPublishStatus::InternalError,
                "创建 MQTT 发布等待对象失败: " +
                std::string(e.what())
            );
        }


        // ==================================================
        // 5. 提交 MQTT QoS 1 消息
        // ==================================================

        int message_id = 0;

        int rc = MOSQ_ERR_SUCCESS;

        {
            std::lock_guard<std::mutex> lock(
                pending_publish_mutex_
            );

            // 必须在 pending_publish_mutex_ 内检查停止状态。
            //
            // 防止 stop() 先完成待确认消息清理，
            // 然后 publish() 才插入一个新的等待对象。

            if (stopping_.load())
            {
                return makePublishResult(
                    MqttPublishStatus::InternalError,
                    "MQTT 客户端正在停止"
                );
            }

            if (!connected_.load())
            {
                return makePublishResult(
                    MqttPublishStatus::NotConnected,
                    "MQTT Broker 当前未连接"
                );
            }


            const auto slot = std::find(
                pending_publishes_.begin(), pending_publishes_.end(), nullptr
            );
            if (slot == pending_publishes_.end())
            {
                return makePublishResult(
                    MqttPublishStatus::PublishFailed,
                    "未确认的 MQTT 发布已达上限，请稍后重试"
                );
            }

            rc = mosquitto_publish(
                mosquitto_.get(),
                &message_id,
                topic.c_str(),
                static_cast<int>(payload.size()),
                payload.data(),
                1,
                false
            );

            if (rc != MOSQ_ERR_SUCCESS)
            {
                const auto status =
                    rc == MOSQ_ERR_NO_CONN
                    ? MqttPublishStatus::NotConnected
                    : MqttPublishStatus::PublishFailed;

                return makePublishResult(
                    status,
                    "提交 MQTT 消息失败: " +
                    std::string(mosquitto_strerror(rc))
                );
            }


            // ==================================================
            // 关键：建立 MID 与等待对象的映射
            // ==================================================
            //
            // 网络线程的 PUBACK 回调需要获取这条映射。
            //
            // 当前锁保证：
            //
            // 1. mosquitto_publish() 已经返回 MID。
            // 2. 映射已经插入。
            // 3. PUBACK 回调才能获取 pending 对象。
            //
            // 避免 PUBACK 到达过早而找不到对应消息。
            //
            // ==================================================

            pending->message_id = message_id;
            // MID 回绕后，迟到的 ACK 无法区分两个同号请求。
            // 两者都交给上层重试，各自保留额度直至 ACK 或销毁客户端。
            for (const auto& prior : pending_publishes_)
            {
                if (prior && prior->message_id == message_id)
                {
                    constexpr auto status = MqttPublishStatus::PublishFailed;
                    completePendingPublish(prior, status, "MQTT MID 重复");
                    completePendingPublish(pending, status, "MQTT MID 重复");
                }
            }
            *slot = pending;
        }


        log(
            ::industrial_gateway::common::LogLevel::Debug,
            "MQTT 消息已提交，MID=" +
            std::to_string(message_id)
        );


        // ==================================================
        // 6. 等待 PUBACK
        // ==================================================

        MqttPublishResult result;

        {
            std::unique_lock<std::mutex> lock(
                pending->mutex
            );

            const bool completed =
                pending->condition.wait_for(
                    lock,
                    acknowledgement_timeout,
                    [&pending]()
                    {
                        return pending->completed;
                    }
                );

            if (!completed)
            {
                // 这里只代表应用层停止等待 PUBACK，不代表 Mosquitto
                // 已经撤销底层 QoS 1 消息。Broker 可能已经收到消息，
                // 也可能稍后才返回 PUBACK。因此上层必须保留 SQLite 记录，
                // 使用稳定 message_id 重试，并由平台按 message_id 幂等去重。
                pending->completed = true;

                pending->result = makePublishResult(
                    MqttPublishStatus::AcknowledgementTimeout,
                    "等待 MQTT PUBACK 超时，MID=" +
                    std::to_string(message_id)
                );
            }

            result = pending->result;
        }


        // ==================================================
        // 7. 返回发布结果。超时不撤销 libmosquitto 队列中的消息。
        // ==================================================

        if (result.status == MqttPublishStatus::Acknowledged)
        {
            log(
                ::industrial_gateway::common::LogLevel::Debug,
                "MQTT PUBACK 已确认，MID=" +
                std::to_string(message_id)
            );
        }
        else
        {
            log(
                ::industrial_gateway::common::LogLevel::Warn,
                "MQTT 消息发布未得到确认，MID=" +
                std::to_string(message_id) +
                "，状态=" +
                std::to_string(static_cast<int>(result.status))
            );
        }

        return result;
    }


    // ==================================================
    // MQTT 消息订阅
    // ==================================================

    bool subscribe(
        const std::string& topic_filter,
        int qos,
        ::industrial_gateway::application::ports::
            MqttMessageHandler message_handler,
        ::industrial_gateway::application::ports::
            MqttSubscriptionResultHandler result_handler,
        std::string& error_message
    )
    {
        error_message.clear();


        // ==================================================
        // 1. 检查 MQTT 是否启用
        // ==================================================

        if (!config_.enabled)
        {
            error_message =
                "MQTT 已禁用，不能登记订阅";

            return false;
        }


        // ==================================================
        // 2. 校验 Topic Filter
        // ==================================================

        // 订阅意图允许在 start() 前和 stop() 后登记。
        if (!library_->acquire(error_message))
        {
            return false;
        }
        const int topic_rc = mosquitto_sub_topic_check(topic_filter.c_str());
        library_->release();

        if (
            topic_filter.empty() ||
            topic_filter.find('\0') != std::string::npos ||
            topic_rc != MOSQ_ERR_SUCCESS
        )
        {
            error_message =
                "MQTT 订阅 Topic Filter 不合法";

            return false;
        }


        // ==================================================
        // 3. 校验 QoS
        // ==================================================

        if (qos < 0 || qos > 2)
        {
            error_message =
                "MQTT 订阅 QoS 必须为 0、1 或 2";

            return false;
        }


        // ==================================================
        // 4. 校验消息回调
        // ==================================================

        if (!message_handler)
        {
            error_message =
                "MQTT 消息接收回调不能为空";

            return false;
        }


        // ==================================================
        // 5. 登记订阅意图
        // ==================================================

        {
            std::lock_guard<std::mutex> lock(
                subscription_mutex_
            );

            if (stopping_.load())
            {
                error_message =
                    "MQTT 客户端正在停止，不能登记订阅";

                return false;
            }


            // 相同 Topic Filter 使用最新配置覆盖。
            SubscriptionEntry entry;

            entry.qos = qos;

            entry.message_handler =
                std::move(message_handler);

            entry.result_handler =
                std::move(result_handler);

            entry.generation =
                ++next_subscription_generation_;


            subscriptions_[topic_filter] =
                std::move(entry);


            // 将 Topic 加入待处理队列。
            //
            // 如果网络线程尚未启动，
            // 订阅意图会保留到后续连接成功。
            //
            // ==================================================

            enqueueSubscriptionLocked(topic_filter);
        }


        log(
            ::industrial_gateway::common::LogLevel::Info,
            "MQTT 订阅意图登记成功: " +
            topic_filter
        );

        return true;
    }


private:

    // ==================================================
    // 一、发布等待对象
    // ==================================================

    struct PendingPublish
    {
        int message_id = 0;

        std::mutex mutex;

        std::condition_variable condition;

        bool completed = false;

        ::industrial_gateway::application::ports::
            MqttPublishResult result;
    };


    // ==================================================
    // 二、订阅信息
    // ==================================================

    struct SubscriptionEntry
    {
        int qos = 0;

        ::industrial_gateway::application::ports::
            MqttMessageHandler message_handler;

        ::industrial_gateway::application::ports::
            MqttSubscriptionResultHandler result_handler;

        std::uint64_t generation = 0;
    };


    // ==================================================
    // 三、待确认订阅
    // ==================================================

    struct PendingSubscription
    {
        std::string topic_filter;

        std::uint64_t generation = 0;
    };


    // ==================================================
    // 四、发布操作生命周期保护
    // ==================================================

    class ActivePublishGuard final
    {
    public:

        explicit ActivePublishGuard(Impl& owner)
            : owner_(owner)
        {
        }

        ~ActivePublishGuard()
        {
            owner_.endActivePublish();
        }

        ActivePublishGuard(const ActivePublishGuard&) = delete;

        ActivePublishGuard& operator=(
            const ActivePublishGuard&
        ) = delete;

    private:

        Impl& owner_;
    };


    // ==================================================
    // 五、生成发布结果
    // ==================================================

    static ::industrial_gateway::application::ports::
        MqttPublishResult makePublishResult(
            ::industrial_gateway::application::ports::
                MqttPublishStatus status,
            std::string error_message = {}
        )
    {
        ::industrial_gateway::application::ports::
            MqttPublishResult result;

        result.status = status;

        result.error_message =
            std::move(error_message);

        return result;
    }


    // ==================================================
    // 六、日志输出
    // ==================================================

    void log(
        ::industrial_gateway::common::LogLevel level,
        std::string_view message
    ) noexcept
    {
        try
        {
            logger_.log(
                level,
                kLogComponent,
                message
            );
        }
        catch (...)
        {
            // 日志异常不能传播到 MQTT 回调边界。
        }
    }


    // ==================================================
    // 八、发布操作注册
    // ==================================================

    bool beginActivePublish()
    {
        std::lock_guard<std::mutex> lock(
            active_publish_mutex_
        );

        if (
            !started_.load() ||
            stopping_.load()
        )
        {
            return false;
        }

        ++active_publish_calls_;

        return true;
    }


    // ==================================================
    // 九、发布操作结束
    // ==================================================

    void endActivePublish() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(
                active_publish_mutex_
            );

            if (active_publish_calls_ > 0)
            {
                --active_publish_calls_;
            }
        }

        active_publish_cv_.notify_all();
    }


    // ==================================================
    // 十、完成单条发布等待
    // ==================================================

    static void completePendingPublish(
        const std::shared_ptr<PendingPublish>& pending,
        ::industrial_gateway::application::ports::
            MqttPublishStatus status,
        std::string_view error_message = {}
    ) noexcept
    {
        if (!pending)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(
                pending->mutex
            );

            // 已经超时或被其他事件完成时，
            // 不允许覆盖原有结果。
            if (pending->completed)
            {
                return;
            }

            pending->result.status = status;

            // 错误信息分配失败不能阻止等待线程被唤醒。
            // 状态码仍然有效，错误文本退化为空字符串。
            try
            {
                if (error_message.empty())
                {
                    pending->result.error_message.clear();
                }
                else
                {
                    pending->result.error_message.assign(
                        error_message.data(),
                        error_message.size()
                    );
                }
            }
            catch (...)
            {
                pending->result.error_message.clear();
            }

            pending->completed = true;
        }

        pending->condition.notify_all();
    }


    // ==================================================
    // 十一、完成全部等待中的发布
    // ==================================================

    void failPendingPublishes(
        ::industrial_gateway::application::ports::
            MqttPublishStatus status,
        std::string_view error_message
    ) noexcept
    {
        decltype(pending_publishes_) pending_list;

        {
            std::lock_guard<std::mutex> lock(
                pending_publish_mutex_
            );

            pending_list = pending_publishes_;
        }


        // 在锁外唤醒等待线程，避免持有全局发布映射锁执行逐条通知。
        for (const auto& entry : pending_list)
        {
            completePendingPublish(
                entry,
                status,
                error_message
            );
        }
    }


    // ==================================================
    // 十二、PUBACK 回调处理
    // ==================================================

    void handlePublishAcknowledgement(
        int message_id
    )
    {
        std::shared_ptr<PendingPublish> pending;

        {
            std::lock_guard<std::mutex> lock(
                pending_publish_mutex_
            );

            const auto it = std::find_if(
                pending_publishes_.begin(), pending_publishes_.end(),
                [message_id](const auto& entry)
                {
                    return entry && entry->message_id == message_id;
                }
            );

            if (it == pending_publishes_.end())
            {
                return;
            }

            pending = std::move(*it);
        }


        completePendingPublish(
            pending,
            ::industrial_gateway::application::ports::
                MqttPublishStatus::Acknowledged
        );
    }


    // ==================================================
    // 十三、登记待处理订阅
    // ==================================================

    void enqueueSubscriptionLocked(
        const std::string& topic_filter
    )
    {
        const auto inserted =
            queued_subscriptions_.insert(topic_filter);

        if (!inserted.second)
        {
            return;
        }

        subscription_queue_.push_back(topic_filter);
    }


    // ==================================================
    // 十四、连接成功后恢复所有订阅
    // ==================================================

    void enqueueAllSubscriptions()
    {
        std::lock_guard<std::mutex> lock(
            subscription_mutex_
        );

        if (stopping_.load())
        {
            return;
        }

        for (const auto& entry : subscriptions_)
        {
            enqueueSubscriptionLocked(entry.first);
        }
    }


    // ==================================================
    // 十五、处理待发送订阅命令
    // ==================================================
    //
    // 仅允许 MQTT 网络线程调用。
    //
    // ==================================================

    void processSubscriptionQueue()
    {
        if (
            !connected_.load() ||
            stopping_.load()
        )
        {
            return;
        }

        while (!stopping_.load())
        {
            std::string topic_filter;

            SubscriptionEntry subscription;

            {
                std::lock_guard<std::mutex> lock(
                    subscription_mutex_
                );

                if (subscription_queue_.empty())
                {
                    return;
                }

                topic_filter =
                    std::move(subscription_queue_.front());

                subscription_queue_.pop_front();

                queued_subscriptions_.erase(topic_filter);

                const auto it =
                    subscriptions_.find(topic_filter);

                if (it == subscriptions_.end())
                {
                    continue;
                }

                subscription = it->second;
            }


            // ==================================================
            // 执行实际 SUBSCRIBE
            // ==================================================

            int message_id = 0;

            const int rc = mosquitto_subscribe(
                mosquitto_.get(),
                &message_id,
                topic_filter.c_str(),
                subscription.qos
            );


            if (rc != MOSQ_ERR_SUCCESS)
            {
                ::industrial_gateway::application::ports::
                    MqttSubscriptionResult result;

                result.status =
                    ::industrial_gateway::application::ports::
                        MqttSubscriptionStatus::RequestFailed;

                result.error_message =
                    "提交 MQTT 订阅请求失败: " +
                    std::string(mosquitto_strerror(rc));


                invokeSubscriptionResult(
                    subscription.result_handler,
                    result
                );


                log(
                    ::industrial_gateway::common::LogLevel::Error,
                    "MQTT SUBSCRIBE 请求失败: " +
                    topic_filter +
                    "，错误: " +
                    result.error_message
                );


                if (rc == MOSQ_ERR_NO_CONN)
                {
                    reconnect_requested_.store(true);
                }

                continue;
            }


            // ==================================================
            // 记录 MID 与订阅信息
            // ==================================================
            //
            // SUBACK 回调通过 MID 找到对应订阅。
            //
            // 这里仅由网络线程访问 pending_subscriptions_，
            // 不需要额外加锁。
            //
            // ==================================================

            pending_subscriptions_[message_id] =
                PendingSubscription{
                    topic_filter,
                    subscription.generation
                };


            log(
                ::industrial_gateway::common::LogLevel::Debug,
                "MQTT SUBSCRIBE 请求已提交: " +
                topic_filter +
                "，MID=" +
                std::to_string(message_id)
            );
        }
    }


    // ==================================================
    // 十六、执行订阅结果回调
    // ==================================================

    // 订阅结果回调同样由 MQTT 网络线程同步执行，调用方必须快速返回。
    void invokeSubscriptionResult(
        const ::industrial_gateway::application::ports::
            MqttSubscriptionResultHandler& handler,
        const ::industrial_gateway::application::ports::
            MqttSubscriptionResult& result
    ) noexcept
    {
        if (!handler)
        {
            return;
        }

        try
        {
            handler(result);
        }
        catch (const std::exception& e)
        {
            log(
                ::industrial_gateway::common::LogLevel::Error,
                "MQTT 订阅结果回调发生异常"
            );

            log(
                ::industrial_gateway::common::LogLevel::Error,
                e.what()
            );
        }
        catch (...)
        {
            log(
                ::industrial_gateway::common::LogLevel::Error,
                "MQTT 订阅结果回调发生未知异常"
            );
        }
    }


    // ==================================================
    // 十七、SUBACK 回调处理
    // ==================================================

    void handleSubscribeAcknowledgement(
        int message_id,
        int qos_count,
        const int* granted_qos
    )
    {
        const auto pending_it =
            pending_subscriptions_.find(message_id);

        if (pending_it == pending_subscriptions_.end())
        {
            return;
        }


        const PendingSubscription pending =
            pending_it->second;

        pending_subscriptions_.erase(pending_it);


        ::industrial_gateway::application::ports::
            MqttSubscriptionResult result;


        // ==================================================
        // 检查 Broker 返回的订阅结果
        // ==================================================

        if (
            qos_count != 1 ||
            granted_qos == nullptr
        )
        {
            result.status =
                ::industrial_gateway::application::ports::
                    MqttSubscriptionStatus::Rejected;

            result.error_message =
                "Broker 返回了无效的 SUBACK 结果";
        }
        else if (granted_qos[0] == 0x80)
        {
            result.status =
                ::industrial_gateway::application::ports::
                    MqttSubscriptionStatus::Rejected;

            result.error_message =
                "Broker 拒绝 MQTT 订阅请求";
        }
        else if (
            granted_qos[0] < 0 ||
            granted_qos[0] > 2
        )
        {
            result.status =
                ::industrial_gateway::application::ports::
                    MqttSubscriptionStatus::Rejected;

            result.error_message =
                "Broker 返回了不合法的授权 QoS";
        }
        else
        {
            result.status =
                ::industrial_gateway::application::ports::
                    MqttSubscriptionStatus::Acknowledged;

            result.granted_qos = granted_qos[0];
        }


        // ==================================================
        // 获取当前订阅结果回调
        // ==================================================
        //
        // 如果相同 Topic Filter 已经被重新注册，
        // 旧一代 SUBACK 不能通知旧回调。
        //
        // ==================================================

        ::industrial_gateway::application::ports::
            MqttSubscriptionResultHandler handler;

        {
            std::lock_guard<std::mutex> lock(
                subscription_mutex_
            );

            const auto it =
                subscriptions_.find(pending.topic_filter);

            if (
                it != subscriptions_.end() &&
                it->second.generation == pending.generation
            )
            {
                handler = it->second.result_handler;
            }
        }


        if (
            result.status ==
            ::industrial_gateway::application::ports::
                MqttSubscriptionStatus::Acknowledged
        )
        {
            log(
                ::industrial_gateway::common::LogLevel::Info,
                "MQTT 订阅确认成功: " +
                pending.topic_filter +
                "，授权 QoS=" +
                std::to_string(result.granted_qos)
            );
        }
        else
        {
            log(
                ::industrial_gateway::common::LogLevel::Warn,
                "MQTT 订阅确认失败: " +
                pending.topic_filter +
                "，" +
                result.error_message
            );
        }


        invokeSubscriptionResult(
            handler,
            result
        );
    }


    // ==================================================
    // 十八、MQTT 消息接收处理
    // ==================================================

    void handleMessage(
        const struct mosquitto_message* raw_message
    )
    {
        if (
            raw_message == nullptr ||
            raw_message->topic == nullptr ||
            raw_message->payloadlen < 0
        )
        {
            log(
                ::industrial_gateway::common::LogLevel::Error,
                "收到不合法的 MQTT 消息"
            );

            return;
        }


        try
        {
            // ==================================================
            // 1. 构建应用层消息对象
            // ==================================================

            ::industrial_gateway::application::ports::
                MqttMessage message;

            message.topic = raw_message->topic;

            if (
                raw_message->payloadlen > 0 &&
                raw_message->payload != nullptr
            )
            {
                message.payload = std::string(
                    static_cast<const char*>(
                        raw_message->payload
                    ),
                    static_cast<std::size_t>(
                        raw_message->payloadlen
                    )
                );
            }

            message.qos = raw_message->qos;

            message.retained =
                raw_message->retain != 0;


            // ==================================================
            // 2. 查找匹配的订阅回调
            // ==================================================

            std::vector<
                ::industrial_gateway::application::ports::
                    MqttMessageHandler
            > handlers;

            {
                std::lock_guard<std::mutex> lock(
                    subscription_mutex_
                );

                for (const auto& entry : subscriptions_)
                {
                    bool matches = false;

                    const int rc = mosquitto_topic_matches_sub(
                        entry.first.c_str(),
                        message.topic.c_str(),
                        &matches
                    );

                    if (
                        rc == MOSQ_ERR_SUCCESS &&
                        matches
                    )
                    {
                        handlers.push_back(
                            entry.second.message_handler
                        );
                    }
                }
            }


            // ==================================================
            // 3. 在不持有内部锁的情况下执行回调
            // ==================================================
            // 注意：这些回调仍然运行在 MQTT 网络线程中。
            // 回调必须短小、非阻塞；复杂 JSON 解析、配置应用、
            // 数据库操作和 Connector 生命周期管理应投递到应用层队列，
            // 不能在这里同步执行，否则会阻塞 MQTT 网络收发和 PUBACK。

            for (const auto& handler : handlers)
            {
                if (!handler)
                {
                    continue;
                }

                try
                {
                    handler(message);
                }
                catch (const std::exception& e)
                {
                    log(
                        ::industrial_gateway::common::LogLevel::Error,
                        "MQTT 消息回调发生异常"
                    );

                    log(
                        ::industrial_gateway::common::LogLevel::Error,
                        e.what()
                    );
                }
                catch (...)
                {
                    log(
                        ::industrial_gateway::common::LogLevel::Error,
                        "MQTT 消息回调发生未知异常"
                    );
                }
            }
        }
        catch (const std::exception& e)
        {
            log(
                ::industrial_gateway::common::LogLevel::Error,
                "处理 MQTT 接收消息失败"
            );

            log(
                ::industrial_gateway::common::LogLevel::Error,
                e.what()
            );
        }
        catch (...)
        {
            log(
                ::industrial_gateway::common::LogLevel::Error,
                "处理 MQTT 接收消息发生未知异常"
            );
        }
    }


    // ==================================================
    // 十九、连接成功回调处理
    // ==================================================

    void handleConnect(int result)
    {
        if (result != 0)
        {
            connected_.store(false);

            const std::string error_message =
                "MQTT Broker 拒绝连接，CONNACK=" +
                std::to_string(result);

            failPendingPublishes(
                ::industrial_gateway::application::ports::
                    MqttPublishStatus::ConnectionLost,
                error_message
            );

            pending_subscriptions_.clear();

            if (result == kConnackServerUnavailable)
            {
                // MQTT 3.1.1 Server Unavailable 属于可恢复拒绝。
                reconnect_requested_.store(true);

                log(
                    ::industrial_gateway::common::LogLevel::Warn,
                    error_message + "，将执行退避重连"
                );

                return;
            }

            // MQTT 3.1.1 的其他 CONNACK 拒绝原因通常表示协议、
            // Client ID、用户名/密码或授权配置错误，不应无限重连。
            failure_.record(error_message);
            network_failed_.store(true);
            requestStop();
            reconnect_requested_.store(false);

            log(
                ::industrial_gateway::common::LogLevel::Critical,
                error_message + "，请检查 MQTT 协议、Client ID 和认证配置"
            );

            return;
        }


        connected_.store(true);

        reconnect_requested_.store(false);


        // 连接成功后重新登记所有订阅。
        enqueueAllSubscriptions();


        log(
            ::industrial_gateway::common::LogLevel::Info,
            "MQTT Broker 连接成功"
        );
    }

    // ==================================================
    // 二十、连接断开回调处理
    // ==================================================

    void handleDisconnect(int result)
    {
        connected_.store(false);

        pending_subscriptions_.clear();

        failPendingPublishes(
            ::industrial_gateway::application::ports::
                MqttPublishStatus::ConnectionLost,
            "MQTT Broker 连接断开"
        );

        if (stopping_.load())
        {
            // 这是本客户端主动停止时的 DISCONNECT 回调。
            // 通知 networkLoop() 可以退出优雅断开阶段。
            disconnect_completed_.store(true);

            log(
                ::industrial_gateway::common::LogLevel::Info,
                "MQTT Broker 已断开，result=" +
                std::to_string(result)
            );

            return;
        }

        reconnect_requested_.store(true);

        log(
            ::industrial_gateway::common::LogLevel::Warn,
            "MQTT Broker 连接断开，准备重新连接，错误码=" +
            std::to_string(result)
        );
    }


    // ==================================================
    // 二十一、网络线程执行优雅断开
    // ==================================================
    //
    // 只能由 MQTT 网络线程调用。
    // mosquitto_disconnect() 仅发起 DISCONNECT，随后仍需执行
    // mosquitto_loop()，让 libmosquitto 将报文写入网络并触发断开回调。
    //
    // ==================================================

    void performGracefulDisconnect() noexcept
    {
        if (mosquitto_ == nullptr)
        {
            disconnect_completed_.store(true);
            return;
        }

        disconnect_completed_.store(false);

        const int disconnect_rc = mosquitto_disconnect(
            mosquitto_.get()
        );

        if (disconnect_rc == MOSQ_ERR_NO_CONN)
        {
            disconnect_completed_.store(true);
            return;
        }

        if (disconnect_rc != MOSQ_ERR_SUCCESS)
        {
            log(
                ::industrial_gateway::common::LogLevel::Warn,
                "请求 MQTT 优雅断开失败"
            );

            log(
                ::industrial_gateway::common::LogLevel::Warn,
                mosquitto_strerror(disconnect_rc)
            );

            disconnect_completed_.store(true);
            return;
        }

        while (!disconnect_completed_.load())
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                disconnect_deadline_ - std::chrono::steady_clock::now()
            );
            if (remaining.count() <= 0)
            {
                break;
            }

            const int loop_rc = mosquitto_loop(
                mosquitto_.get(),
                static_cast<int>(std::min(
                    remaining, std::chrono::milliseconds{kNetworkLoopTimeoutMs}
                ).count()),
                kNetworkLoopMaxPackets
            );

            if (disconnect_completed_.load())
            {
                break;
            }

            if (loop_rc != MOSQ_ERR_SUCCESS)
            {
                if (loop_rc != MOSQ_ERR_NO_CONN)
                {
                    log(
                        ::industrial_gateway::common::LogLevel::Warn,
                        "处理 MQTT DISCONNECT 时网络循环异常"
                    );

                    log(
                        ::industrial_gateway::common::LogLevel::Warn,
                        mosquitto_strerror(loop_rc)
                    );
                }

                break;
            }
        }

        if (!disconnect_completed_.load())
        {
            log(
                ::industrial_gateway::common::LogLevel::Warn,
                "等待 MQTT DISCONNECT 处理超时，将继续释放客户端资源"
            );
        }
    }


    // ==================================================
    // 二十二、标记网络线程不可恢复失败
    // ==================================================

    void markNetworkFailure(
        std::string_view reason, std::string_view detail = {}
    ) noexcept
    {
        failure_.record(reason, detail);
        network_failed_.store(true);
        requestStop();
        connected_.store(false);
        reconnect_requested_.store(false);

        pending_subscriptions_.clear();

        failPendingPublishes(
            ::industrial_gateway::application::ports::
                MqttPublishStatus::InternalError,
            reason
        );

        log(
            ::industrial_gateway::common::LogLevel::Critical,
            reason
        );

    }


    // ==================================================
    // 二十三、等待重连
    // ==================================================

    void requestStop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(connection_->mutex);
            if (!stopping_.load())
            {
                disconnect_deadline_ = std::chrono::steady_clock::now() +
                    kGracefulDisconnectTimeout;
                stopping_.store(true);
            }
        }
        connection_->changed.notify_all();
    }

    bool waitBeforeReconnect(
        std::chrono::milliseconds delay
    )
    {
        std::unique_lock<std::mutex> lock(
            connection_->mutex
        );

        connection_->changed.wait_for(
            lock,
            delay,
            [this]()
            {
                return stopping_.load();
            }
        );

        return !stopping_.load();
    }

    struct ConnectResult
    {
        int code = MOSQ_ERR_SUCCESS;
        int system_error = 0;
    };

    // 跨 stop()/start() 复用。即使旧连接仍在系统调用中，每个实例也至多
    // 有一个连接任务；新的网络线程可以等待它退出，也可以被 stop() 唤醒。
    struct ConnectionState
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool busy = false;
        ConnectResult result;
    };

    std::optional<ConnectResult> connectUntilStopped(bool reconnect)
    {
        const auto state = connection_;
        std::unique_lock<std::mutex> lock(state->mutex);
        const auto ready = [&] { return !state->busy || stopping_.load(); };
        state->changed.wait(lock, ready);
        if (stopping_.load())
        {
            return std::nullopt;
        }

        // 阻塞任务只持有连接参数、旧句柄和共享状态。它不访问 Impl、
        // logger 或业务回调，只有网络线程重新绑定 userdata 后才处理 MQTT 包。
        mosquitto_user_data_set(mosquitto_.get(), nullptr);
        std::thread worker([
            state, client = mosquitto_, host = config_.host, port = config_.port,
            keep_alive = config_.keep_alive, reconnect
        ]() mutable {
            const int rc = reconnect
                ? mosquitto_reconnect(client.get())
                : mosquitto_connect(client.get(), host.c_str(), port, keep_alive);
            const ConnectResult result{rc, errno};
            client.reset();
            {
                std::lock_guard<std::mutex> complete(state->mutex);
                state->result = result;
                state->busy = false;
            }
            state->changed.notify_all();
        });
        state->busy = true;
        state->changed.wait(lock, ready);
        if (state->busy)
        {
            // stop() 已经唤醒网络线程。系统调用返回后由 worker 释放自己的引用。
            worker.detach();
            return std::nullopt;
        }

        const auto result = state->result;
        lock.unlock();
        worker.join();
        if (stopping_.load())
        {
            return std::nullopt;
        }
        mosquitto_user_data_set(mosquitto_.get(), this);
        return result;
    }


    // ==================================================
    // 二十四、网络循环
    // ==================================================
    //
    // MQTT 网络线程唯一入口。
    //
    // 负责：
    //
    // 1. 初始连接与手动重连。
    // 2. MQTT 网络 I/O。
    // 3. SUBSCRIBE 命令执行。
    // 4. PUBACK / SUBACK 回调。
    // 5. 优雅断开连接。
    //
    // ==================================================

    void networkLoop() noexcept
    {
        gCurrentMqttNetworkInstance = this;

        std::chrono::milliseconds reconnect_delay =
            kInitialReconnectDelay;

        // mosquitto_reconnect() 只能在成功调用过 mosquitto_connect()
        // 后使用。初次连接失败时继续调用 mosquitto_connect()。
        bool connect_parameters_initialized = false;

        bool connect_needed = true;

        // 第一次连接立即执行；后续连接尝试先等待退避时间。
        bool wait_before_connect = false;

        try
        {
            while (!stopping_.load())
            {
                // ==================================================
                // 1. 建立初始连接或执行重连
                // ==================================================

                if (connect_needed)
                {
                    if (wait_before_connect)
                    {
                        if (!waitBeforeReconnect(reconnect_delay))
                        {
                            break;
                        }

                        reconnect_delay = std::min(
                            reconnect_delay * 2,
                            kMaxReconnectDelay
                        );
                    }

                    if (stopping_.load())
                    {
                        break;
                    }

                    const auto result = connectUntilStopped(connect_parameters_initialized);
                    if (!result)
                    {
                        // 连接任务可能仍在使用旧句柄，不能再对它执行 disconnect/loop。
                        gCurrentMqttNetworkInstance = nullptr;
                        return;
                    }
                    const int connect_rc = result->code;

                    if (connect_rc != MOSQ_ERR_SUCCESS)
                    {
                        if (isFatalMosquittoError(connect_rc, result->system_error))
                        {
                            markNetworkFailure(
                                "MQTT 连接请求发生不可恢复错误", mosquitto_strerror(connect_rc)
                            );

                            log(
                                ::industrial_gateway::common::LogLevel::Critical,
                                mosquitto_strerror(connect_rc)
                            );

                            break;
                        }

                        log(
                            ::industrial_gateway::common::LogLevel::Warn,
                            "MQTT 连接请求失败，将在退避后重试"
                        );

                        log(
                            ::industrial_gateway::common::LogLevel::Warn,
                            mosquitto_strerror(connect_rc)
                        );

                        wait_before_connect = true;
                        continue;
                    }

                    connect_parameters_initialized = true;
                    connect_needed = false;
                    wait_before_connect = false;

                    log(
                        ::industrial_gateway::common::LogLevel::Info,
                        "MQTT TCP 连接请求已建立，等待 Broker CONNACK"
                    );
                }


                if (stopping_.load())
                {
                    break;
                }


                // ==================================================
                // 2. 处理待订阅命令
                // ==================================================

                if (connected_.load())
                {
                    processSubscriptionQueue();
                }


                if (stopping_.load())
                {
                    break;
                }


                // ==================================================
                // 3. 执行 Mosquitto 网络循环
                // ==================================================

                const int loop_rc = mosquitto_loop(
                    mosquitto_.get(),
                    kNetworkLoopTimeoutMs,
                    kNetworkLoopMaxPackets
                );


                if (stopping_.load() || network_failed_.load())
                {
                    break;
                }


                // ==================================================
                // 4. 判断是否需要重连
                // ==================================================

                bool should_reconnect =
                    reconnect_requested_.exchange(false);

                if (loop_rc != MOSQ_ERR_SUCCESS)
                {
                    if (isFatalMosquittoError(loop_rc))
                    {
                        markNetworkFailure(
                            "MQTT 网络循环发生不可恢复错误", mosquitto_strerror(loop_rc)
                        );

                        log(
                            ::industrial_gateway::common::LogLevel::Critical,
                            mosquitto_strerror(loop_rc)
                        );

                        break;
                    }

                    should_reconnect = true;

                    log(
                        ::industrial_gateway::common::LogLevel::Warn,
                        "MQTT 网络循环发生可恢复错误，将执行重连"
                    );

                    log(
                        ::industrial_gateway::common::LogLevel::Warn,
                        mosquitto_strerror(loop_rc)
                    );
                }


                if (should_reconnect)
                {
                    connected_.store(false);

                    // 旧连接上的 SUBSCRIBE MID 已失效。
                    // 清空映射，避免重连后 MID 复用时匹配到旧请求。
                    pending_subscriptions_.clear();

                    failPendingPublishes(
                        ::industrial_gateway::application::ports::
                            MqttPublishStatus::ConnectionLost,
                        "MQTT 连接丢失，等待重新连接"
                    );

                    connect_needed = true;
                    wait_before_connect = true;

                    continue;
                }


                // Broker 确认连接成功后，重置指数退避时间。
                if (connected_.load())
                {
                    reconnect_delay = kInitialReconnectDelay;
                }
            }
        }
        catch (const std::exception& e)
        {
            markNetworkFailure(
                "MQTT 网络线程发生异常", e.what()
            );

            log(
                ::industrial_gateway::common::LogLevel::Critical,
                e.what()
            );
        }
        catch (...)
        {
            markNetworkFailure(
                "MQTT 网络线程发生未知异常"
            );
        }


        // 停止请求由网络线程自己完成 DISCONNECT，随后继续驱动网络循环，
        // 确保 DISCONNECT 有机会被写出并触发断开回调。
        if (stopping_.load())
        {
            performGracefulDisconnect();
        }

        connected_.store(false);

        gCurrentMqttNetworkInstance = nullptr;
    }


    // ==================================================
    // 二十五、Mosquitto C 回调
    // ==================================================
    //
    // 所有回调都不能允许 C++ 异常穿透 C 接口。
    //
    // ==================================================

    static void onConnect(
        struct mosquitto*,
        void* userdata,
        int result
    ) noexcept
    {
        auto* self = static_cast<Impl*>(userdata);

        if (self == nullptr)
        {
            return;
        }

        try
        {
            self->handleConnect(result);
        }
        catch (...)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT 连接回调时发生异常"
            );
        }
    }


    static void onDisconnect(
        struct mosquitto*,
        void* userdata,
        int result
    ) noexcept
    {
        auto* self = static_cast<Impl*>(userdata);

        if (self == nullptr)
        {
            return;
        }

        try
        {
            self->handleDisconnect(result);
        }
        catch (...)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT 断开回调时发生异常"
            );
        }
    }


    static void onPublish(
        struct mosquitto*,
        void* userdata,
        int message_id
    ) noexcept
    {
        auto* self = static_cast<Impl*>(userdata);

        if (self == nullptr)
        {
            return;
        }

        try
        {
            self->handlePublishAcknowledgement(
                message_id
            );
        }
        catch (...)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT PUBACK 回调时发生异常"
            );
        }
    }


    static void onSubscribe(
        struct mosquitto*,
        void* userdata,
        int message_id,
        int qos_count,
        const int* granted_qos
    ) noexcept
    {
        auto* self = static_cast<Impl*>(userdata);

        if (self == nullptr)
        {
            return;
        }

        try
        {
            self->handleSubscribeAcknowledgement(
                message_id,
                qos_count,
                granted_qos
            );
        }
        catch (...)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT SUBACK 回调时发生异常"
            );
        }
    }


    static void onMessage(
        struct mosquitto*,
        void* userdata,
        const struct mosquitto_message* message
    ) noexcept
    {
        auto* self = static_cast<Impl*>(userdata);

        if (self == nullptr)
        {
            return;
        }

        try
        {
            self->handleMessage(message);
        }
        catch (const std::exception& e)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT 消息回调时发生异常"
            );

            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                e.what()
            );
        }
        catch (...)
        {
            self->log(
                ::industrial_gateway::common::LogLevel::Critical,
                "处理 MQTT 消息回调时发生未知异常"
            );
        }
    }


private:

    // ==================================================
    // 配置和外部依赖
    // ==================================================

    // 保存配置副本。
    ::industrial_gateway::config::MqttConfig config_;

    // 日志对象由外部管理。
    ::industrial_gateway::common::ILogger& logger_;


    // ==================================================
    // Mosquitto 资源
    // ==================================================

    std::shared_ptr<MosquittoLibraryManager> library_ = MosquittoLibraryManager::instance();
    std::shared_ptr<struct mosquitto> mosquitto_;


    // ==================================================
    // 生命周期状态
    // ==================================================

    std::atomic<bool> started_{false};

    std::atomic<bool> stopping_{false};

    std::atomic<bool> connected_{false};

    std::atomic<bool> reconnect_requested_{false};

    // 网络线程发生不可恢复异常后置为 true。
    // 必须由外部 stop() 完成 join 和资源回收后才能重新 start()。
    std::atomic<bool> network_failed_{false};
    std::atomic<bool> network_running_{false};
    ::industrial_gateway::common::RuntimeFailure failure_;

    // 由网络线程的 onDisconnect 回调设置；用于确认主动 DISCONNECT 已处理。
    std::atomic<bool> disconnect_completed_{false};

    // 首次停止请求写入，然后发布 stopping_；本轮网络线程退出前保持不变。
    std::chrono::steady_clock::time_point disconnect_deadline_{};
    std::mutex lifecycle_mutex_;


    // ==================================================
    // MQTT 网络线程
    // ==================================================

    std::thread network_thread_;


    // ==================================================
    // 发布消息同步
    // ==================================================

    std::mutex pending_publish_mutex_;

    std::array<std::shared_ptr<PendingPublish>, kMaxOutstandingPublishes>
        pending_publishes_{};


    // ==================================================
    // 正在执行的 publish() 数量
    // ==================================================

    std::mutex active_publish_mutex_;

    std::condition_variable active_publish_cv_;

    std::size_t active_publish_calls_ = 0;


    // ==================================================
    // 订阅管理
    // ==================================================

    std::mutex subscription_mutex_;

    std::unordered_map<
        std::string,
        SubscriptionEntry
    > subscriptions_;

    std::deque<std::string> subscription_queue_;

    std::unordered_set<std::string> queued_subscriptions_;

    std::uint64_t next_subscription_generation_ = 0;


    // ==================================================
    // SUBACK MID 管理
    // ==================================================
    //
    // 仅由 MQTT 网络线程访问。
    //
    // ==================================================

    std::unordered_map<
        int,
        PendingSubscription
    > pending_subscriptions_;


    // ==================================================
    // 重连等待
    // ==================================================

    std::shared_ptr<ConnectionState> connection_ = std::make_shared<ConnectionState>();

};


// ==================================================
// MosquittoMqttClient 构造函数
// ==================================================

MosquittoMqttClient::MosquittoMqttClient(
    const ::industrial_gateway::config::MqttConfig& config,
    ::industrial_gateway::common::ILogger& logger
)
    : impl_(
        std::make_unique<Impl>(
            config,
            logger
        )
    )
{
}


// ==================================================
// MosquittoMqttClient 析构函数
// ==================================================

MosquittoMqttClient::~MosquittoMqttClient() noexcept
{
    // stop() 可以在网络回调线程中请求停止，但不能在该线程中完成 join。
    // 如果此时继续析构 Impl，网络线程会继续访问已释放对象，形成 UAF。
    // 因此对象必须由外部管理线程销毁。
    if (impl_ && impl_->isCurrentNetworkThread())
    {
        std::terminate();
    }

    stop();
}


// ==================================================
// 启动 MQTT 客户端
// ==================================================

bool MosquittoMqttClient::start(
    std::string& error_message
)
{
    return impl_->start(error_message);
}


// ==================================================
// 停止 MQTT 客户端
// ==================================================

void MosquittoMqttClient::stop() noexcept
{
    if (impl_)
    {
        impl_->stop();
    }
}


// ==================================================
// 查询连接状态
// ==================================================

bool MosquittoMqttClient::isConnected() const noexcept
{
    return impl_->isConnected();
}

bool MosquittoMqttClient::isRunning() const noexcept { return impl_->isRunning(); }
std::string MosquittoMqttClient::failureReason() const { return impl_->failureReason(); }


// ==================================================
// MQTT 消息发布
// ==================================================

::industrial_gateway::application::ports::
    MqttPublishResult MosquittoMqttClient::publish(
        const std::string& topic,
        const std::string& payload,
        std::chrono::milliseconds acknowledgement_timeout
    )
{
    return impl_->publish(
        topic,
        payload,
        acknowledgement_timeout
    );
}


// ==================================================
// MQTT 消息订阅
// ==================================================

bool MosquittoMqttClient::subscribe(
    const std::string& topic_filter,
    int qos,
    ::industrial_gateway::application::ports::
        MqttMessageHandler message_handler,
    ::industrial_gateway::application::ports::
        MqttSubscriptionResultHandler result_handler,
    std::string& error_message
)
{
    return impl_->subscribe(
        topic_filter,
        qos,
        std::move(message_handler),
        std::move(result_handler),
        error_message
    );
}

} // namespace industrial_gateway::infrastructure::mqtt
