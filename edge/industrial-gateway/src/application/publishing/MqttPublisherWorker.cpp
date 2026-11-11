#include "MqttPublisherWorker.hpp"
#include "../messaging/MqttTopics.hpp"
#include "../messaging/ConfigAckSerializer.hpp"
#include "../../infrastructure/concurrency/ConfigAckQueue.hpp"

#include <exception>
#include <stdexcept>

namespace industrial_gateway::application::publishing
{
    namespace
    {
        std::string telemetryTopic(const std::string &gateway_id)
        {
            if (gateway_id.empty() || gateway_id.find_first_of("/+#") != std::string::npos ||
                gateway_id.find('\0') != std::string::npos)
            {
                throw std::invalid_argument("gateway_id 必须是非空的单个 MQTT Topic 层级");
            }
            return messaging::MqttTopics::telemetry(gateway_id);
        }
    } // namespace

    MqttPublisherWorker::MqttPublisherWorker(
        ports::IMessageStorage &message_storage,
        ports::IMqttClient &mqtt_client,
        common::ILogger &logger,
        std::string gateway_id,
        infrastructure::concurrency::ConfigAckQueue* config_acks)
        : message_storage_(message_storage), mqtt_client_(mqtt_client), logger_(logger),
          topic_(telemetryTopic(gateway_id)), config_ack_topic_(messaging::MqttTopics::configAck(gateway_id)),
          config_acks_(config_acks)
    {
    }

    MqttPublisherWorker::~MqttPublisherWorker()
    {
        stop();
    }

    bool MqttPublisherWorker::start(std::string &error_message)
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        error_message.clear();
        if (running_.load())
            return true;
        if (worker_thread_.joinable())
            worker_thread_.join();

        failure_.reset();
        stop_requested_.store(false);
        running_.store(true);
        try
        {
            worker_thread_ = std::thread([this]()
                                         {
            try
            {
                run();
            }
            catch (const std::exception& error)
            {
                failure_.record("MQTT 发布线程异常退出", error.what());
                // 异常路径不删除未确认记录；已确认但未删除的包可能再次发送。
                try
                {
                    log(common::LogLevel::Critical,
                        std::string("发布线程异常退出，数据库记录保留: ") + error.what());
                }
                catch (...) {}
            }
            catch (...)
            {
                failure_.record("MQTT 发布线程因未知异常退出");
                try { log(common::LogLevel::Critical, "发布线程因未知异常退出"); }
                catch (...) {}
            }
            if (!stop_requested_.load())
                failure_.record("MQTT 发布线程未经停止请求退出");
            running_.store(false); });
        }
        catch (const std::exception &error)
        {
            running_.store(false);
            error_message = error.what();
            return false;
        }
        return true;
    }

    void MqttPublisherWorker::stop() noexcept
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            // 与条件变量谓词检查使用同一把锁，避免停止通知丢失。
            std::lock_guard<std::mutex> wait_lock(wait_mutex_);
            stop_requested_.store(true);
        }
        wakeup_.notify_all();
        if (worker_thread_.joinable())
            worker_thread_.join();
    }

    bool MqttPublisherWorker::isRunning() const noexcept
    {
        return running_.load() && !failure_.hasFailure();
    }

    std::string MqttPublisherWorker::failureReason() const { return failure_.reason(); }

    bool MqttPublisherWorker::waitForStop(std::chrono::milliseconds duration)
    {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        return wakeup_.wait_for(lock, duration, [this]()
                                { return stop_requested_.load(); });
    }

    void MqttPublisherWorker::log(common::LogLevel level, const std::string &message) noexcept
    {
        // 日志故障不能中断确认后的清理，也不能使线程逃逸异常。
        try
        {
            logger_.log(level, "MqttPublisherWorker", message);
        }
        catch (...)
        {
        }
    }

    void MqttPublisherWorker::publishOneConfigAck()
    {
        if (!config_acks_ || stop_requested_.load() || !mqtt_client_.isConnected() ||
            std::chrono::steady_clock::now() < next_ack_attempt_) return;
        // 发送期间队首仍占容量，失败保留。tryPeek/popFront 只由此线程调用。
        try
        {
            ::industrial_gateway::config::ConfigApplyResult ack;
            if (!config_acks_->tryPeek(ack)) return;
            std::string payload, error;
            if (!messaging::ConfigAckSerializer::serialize(ack, payload, error))
            {
                log(common::LogLevel::Error, "配置 ACK 无法序列化，移除非法结果: " + error);
                config_acks_->popFront();
                return;
            }
            const auto sent = mqtt_client_.publish(config_ack_topic_, payload, kAcknowledgementTimeout);
            if (sent.isAcknowledged())
                config_acks_->popFront();
            else
            {
                next_ack_attempt_ = std::chrono::steady_clock::now() + kRetryInterval;
                log(common::LogLevel::Warn, "配置 ACK 未确认，保留队首等待重试");
            }
        }
        catch (...)
        {
            next_ack_attempt_ = std::chrono::steady_clock::now() + kRetryInterval;
            log(common::LogLevel::Error, "配置 ACK 发送异常，保留结果等待重试");
        }
    }

    bool MqttPublisherWorker::initializeStorage()
    {
        bool failed = false;
        while (!stop_requested_.load())
        {
            // 遥测数据库持续初始化失败也不能阻止配置回复。
            publishOneConfigAck();
            std::string error;
            if (message_storage_.initialize(error))
            {
                if (failed)
                    log(common::LogLevel::Info, "消息存储初始化已恢复");
                return true;
            }
            if (!failed)
                log(common::LogLevel::Error, "消息存储初始化失败: " + error);
            failed = true;
            if (waitForStop(kRetryInterval))
                break;
        }
        return false;
    }

    bool MqttPublisherWorker::deleteAcknowledged(const std::vector<std::int64_t> &ids)
    {
        if (ids.empty())
            return true;
        bool failed = false;
        for (;;)
        {
            publishOneConfigAck();
            // 即使已请求停止，也尝试清理本批已经确认的 ID 一次。
            std::string error;
            if (message_storage_.deleteBatch(ids, error))
            {
                if (failed)
                    log(common::LogLevel::Info, "已确认消息删除已恢复");
                return true;
            }
            if (!failed)
                log(common::LogLevel::Error, "已确认消息删除失败，优先重试删除: " + error);
            failed = true;
            if (waitForStop(kRetryInterval))
                return false;
            // 回滚失败可能使存储实例失效，重试前恢复初始化。
            if (!initializeStorage())
                return false;
        }
    }

    void MqttPublisherWorker::run()
    {
        log(common::LogLevel::Info, "发布线程启动");
        bool storage_ready = false;
        bool disconnected = false;
        bool query_failed = false;
        bool publish_failed = false;
        std::vector<ports::StoredMessage> batch;
        std::vector<std::int64_t> acknowledged_ids;
        acknowledged_ids.reserve(kBatchSize);

        while (!stop_requested_.load())
        {
            publishOneConfigAck();
            if (!storage_ready)
            {
                if (!initializeStorage())
                    break;
                storage_ready = true;
            }
            if (stop_requested_.load())
                break;
            if (!mqtt_client_.isConnected())
            {
                if (!disconnected)
                    log(common::LogLevel::Warn, "MQTT 未连接，等待客户端恢复连接");
                disconnected = true;
                if (waitForStop(kRetryInterval))
                    break;
                continue;
            }
            if (disconnected)
                log(common::LogLevel::Info, "MQTT 连接已恢复");
            disconnected = false;

            batch.clear();
            std::string error;
            if (!message_storage_.queryBatch(kBatchSize, batch, error))
            {
                if (!query_failed)
                    log(common::LogLevel::Error, "读取待发送消息失败: " + error);
                query_failed = true;
                storage_ready = false;
                if (waitForStop(kRetryInterval))
                    break;
                continue;
            }
            if (query_failed)
                log(common::LogLevel::Info, "待发送消息查询已恢复");
            query_failed = false;
            if (batch.empty())
            {
                if (waitForStop(kIdleInterval))
                    break;
                continue;
            }

            acknowledged_ids.clear();
            bool batch_failed = false;
            for (const auto &message : batch)
            {
                if (stop_requested_.load())
                    break;
                publishOneConfigAck();
                if (stop_requested_.load()) break;
                // publish 内部等待 PUBACK；绝不解析或改写 SQLite 中的完整包。
                const auto result = mqtt_client_.publish(topic_, message.message, kAcknowledgementTimeout);
                if (!result.isAcknowledged())
                {
                    if (!publish_failed)
                    {
                        log(common::LogLevel::Warn,
                            "发布未确认，保留记录并停止本批发送，SQLite ID=" +
                                std::to_string(message.id) + "，状态=" +
                                std::to_string(static_cast<int>(result.status)) + ": " + result.error_message);
                    }
                    publish_failed = true;
                    batch_failed = true;
                    break;
                }
                acknowledged_ids.push_back(message.id);
            }

            // 删除失败时不读取或发布下一批。停止时删除失败则直接退出。
            if (!deleteAcknowledged(acknowledged_ids))
                break;
            if (stop_requested_.load())
                break;
            if (batch_failed)
            {
                // 超时不取消 MQTT 客户端中的旧发送；重试可能重复，平台按 message_id 去重。
                if (waitForStop(kRetryInterval))
                    break;
            }
            else if (publish_failed)
            {
                log(common::LogLevel::Info, "MQTT 消息发布已恢复");
                publish_failed = false;
            }
        }
        log(common::LogLevel::Info, "发布线程停止，剩余消息保留在数据库");
    }

} // namespace industrial_gateway::application::publishing
