#pragma once

#include "industrial_gateway/application/ports/IMqttClient.hpp"
#include "industrial_gateway/common/ILogger.hpp"

#include <memory>
#include <mutex>
#include <string>


namespace industrial_gateway::infrastructure::concurrency
{
class ConfigCommandQueue;
}

namespace industrial_gateway::application::agent
{

// ==================================================
// 网关配置消息接收入口
// ==================================================
//
// 登记当前网关的配置 Topic 订阅，将回调消息复制到 ConfigCommandQueue。
// 不创建工作线程，不解析 JSON，不应用配置，也不在网络回调中发布 ACK。
// MQTT 启动、停止及重连由 MQTT 客户端和启动装配层负责。
// 本类独占登记该配置 Topic 的回调，其他模块不要覆盖相同 Topic 的订阅。
// 队列满、关闭或复制失败时只记录错误，本次消息不会进入处理流程。
// 这不阻止 MQTT 层确认消息，不能依赖 Broker 自动重投；后续平台应按
// 应用层配置 ACK 判断结果并在未确认时重发，本模块不提供持久化重试。
//
// ==================================================

class GatewayAgent final
{
public:
    // gateway_id 必须为非空白的单个 Topic 层级，不含 /、+、# 或空字符。
    // 非法 ID 抛出 invalid_argument；构造时不登记订阅。
    // 外部依赖必须覆盖本对象及其操作的生命周期。
    GatewayAgent(
        ports::IMqttClient& mqtt_client,
        infrastructure::concurrency::ConfigCommandQueue& config_queue,
        common::ILogger& logger,
        std::string gateway_id
    );
    ~GatewayAgent();

    GatewayAgent(const GatewayAgent&) = delete;
    GatewayAgent& operator=(const GatewayAgent&) = delete;
    GatewayAgent(GatewayAgent&&) = delete;
    GatewayAgent& operator=(GatewayAgent&&) = delete;

    // 以 QoS 1 登记订阅，可在 MQTT 客户端 start() 前调用。
    // true 只表示本地登记成功；SUBACK 成功或失败通过结果回调记录日志。
    // 已启动时重复调用返回 true；队列关闭时不能重新启动。
    // 断线重连后的重新订阅使用现有 IMqttClient 行为。
    [[nodiscard]] bool start(std::string& error_message);

    // 停用本地回调，并等待正在执行的回调离开对外部依赖的访问区间。
    // 返回后旧回调不再访问队列或日志；已入队的消息继续保留。
    // 不关闭共享 MQTT 客户端，不关闭配置队列，也不发送 UNSUBSCRIBE：
    // 现有 IMqttClient 没有取消订阅接口，停用期间的消息会被本地忽略。
    // 允许重复停止；再次 start() 会登记一组新的回调。
    // 生命周期操作由控制线程调用，不得从本对象回调或日志调用中重入。
    void stop() noexcept;

    // 表示本地已登记并启用接收，不代表 MQTT 在线或 Broker 已确认订阅。
    [[nodiscard]] bool isStarted() const;
    [[nodiscard]] const std::string& configTopic() const noexcept;

private:
    struct CallbackState;

    ports::IMqttClient& mqtt_client_;
    infrastructure::concurrency::ConfigCommandQueue& config_queue_;
    common::ILogger& logger_;
    const std::string config_topic_;

    mutable std::mutex lifecycle_mutex_;
    std::shared_ptr<CallbackState> callback_state_;
};

} // namespace industrial_gateway::application::agent
