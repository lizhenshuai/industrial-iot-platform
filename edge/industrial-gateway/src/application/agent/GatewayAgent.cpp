#include "GatewayAgent.hpp"

#include "../messaging/MqttTopics.hpp"
#include "../../infrastructure/concurrency/ConfigCommandQueue.hpp"

#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>


namespace industrial_gateway::application::agent
{
namespace
{

std::string makeConfigTopic(const std::string& gateway_id)
{
    if (gateway_id.find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
        gateway_id.find_first_of("/+#") != std::string::npos ||
        gateway_id.find('\0') != std::string::npos)
    {
        throw std::invalid_argument("gateway_id 必须是非空白的单个 MQTT Topic 层级");
    }
    return messaging::MqttTopics::config(gateway_id);
}

} // namespace


// MQTT 客户端仅持有此状态的 weak_ptr，不捕获 GatewayAgent 的 this。
// 独立状态配合互斥锁，确保 stop() 后晚到的回调不会触碰外部依赖。
struct GatewayAgent::CallbackState
{
    CallbackState(infrastructure::concurrency::ConfigCommandQueue& queue,
                  common::ILogger& logger, std::string topic)
        : queue(queue), logger(logger), topic(std::move(topic))
    {
    }

    void log(common::LogLevel level, std::string_view message) noexcept
    {
        try { logger.log(level, "GatewayAgent", message); }
        catch (...) {}
    }

    void onMessage(const ports::MqttMessage& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled) return;
        if (message.topic != topic)
        {
            log(common::LogLevel::Warn, "忽略非当前网关配置 Topic 的消息");
            return;
        }

        try
        {
            using infrastructure::concurrency::ConfigCommandPushStatus;
            switch (queue.tryPush(message))
            {
                case ConfigCommandPushStatus::Accepted:
                    break;
                case ConfigCommandPushStatus::Full:
                    log(common::LogLevel::Error, "配置队列已满，本次配置消息未入队");
                    break;
                case ConfigCommandPushStatus::Closed:
                    log(common::LogLevel::Warn, "配置队列已关闭，本次配置消息未入队");
                    break;
            }
        }
        catch (...)
        {
            // 不记录 payload，避免协议配置中的敏感数据进入日志。
            log(common::LogLevel::Error, "复制配置消息或入队时发生异常");
        }
    }

    void onSubscriptionResult(const ports::MqttSubscriptionResult& result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled) return;
        if (result.isAcknowledged())
        {
            if (result.granted_qos == 0)
                log(common::LogLevel::Warn, "配置 Topic 订阅成功，但 Broker 仅授予 QoS 0");
            else
                log(common::LogLevel::Info, "配置 Topic 订阅成功");
        }
        else
        {
            log(common::LogLevel::Error, "配置 Topic 订阅失败");
            if (!result.error_message.empty())
                log(common::LogLevel::Error, result.error_message);
        }
    }

    void disable()
    {
        std::lock_guard<std::mutex> lock(mutex);
        enabled = false;
    }

    infrastructure::concurrency::ConfigCommandQueue& queue;
    common::ILogger& logger;
    const std::string topic;
    std::mutex mutex;
    bool enabled{true};
};


GatewayAgent::GatewayAgent(
    ports::IMqttClient& mqtt_client,
    infrastructure::concurrency::ConfigCommandQueue& config_queue,
    common::ILogger& logger,
    std::string gateway_id
)
    : mqtt_client_(mqtt_client), config_queue_(config_queue), logger_(logger),
      config_topic_(makeConfigTopic(gateway_id))
{
}

GatewayAgent::~GatewayAgent()
{
    stop();
}

bool GatewayAgent::start(std::string& error_message)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    error_message.clear();
    if (callback_state_) return true;
    if (config_queue_.isClosed())
    {
        error_message = "配置队列已关闭，不能启动 GatewayAgent";
        return false;
    }

    try
    {
        callback_state_ = std::make_shared<CallbackState>(config_queue_, logger_, config_topic_);
        const std::weak_ptr<CallbackState> weak_state = callback_state_;
        // 不持有状态锁调用外部 subscribe，兼容登记期间立即触发的结果回调。
        if (mqtt_client_.subscribe(config_topic_, 1,
                [weak_state](const ports::MqttMessage& message)
                {
                    if (const auto state = weak_state.lock()) state->onMessage(message);
                },
                [weak_state](const ports::MqttSubscriptionResult& result)
                {
                    if (const auto state = weak_state.lock()) state->onSubscriptionResult(result);
                }, error_message))
        {
            return true;
        }
        if (error_message.empty()) error_message = "配置 Topic 订阅登记失败";
    }
    catch (const std::exception& error)
    {
        error_message = std::string("配置 Topic 订阅登记异常: ") + error.what();
    }
    catch (...)
    {
        error_message = "配置 Topic 订阅登记发生未知异常";
    }

    if (callback_state_) callback_state_->disable();
    callback_state_.reset();
    return false;
}

void GatewayAgent::stop() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (callback_state_) callback_state_->disable();
    callback_state_.reset();
}

bool GatewayAgent::isStarted() const
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return static_cast<bool>(callback_state_);
}

const std::string& GatewayAgent::configTopic() const noexcept
{
    return config_topic_;
}

} // namespace industrial_gateway::application::agent
