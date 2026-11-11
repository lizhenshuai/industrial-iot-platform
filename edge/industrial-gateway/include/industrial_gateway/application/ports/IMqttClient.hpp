
#pragma once

#include <chrono>
#include <functional>
#include <string>


namespace industrial_gateway::application::ports
{

// ==================================================
// MQTT 消息发布状态
// ==================================================

enum class MqttPublishStatus
{
    // Broker 已返回 PUBACK
    Acknowledged,

    // MQTT 当前未连接
    NotConnected,

    // MQTT 消息提交失败
    PublishFailed,

    // 等待 PUBACK 超时
    AcknowledgementTimeout,

    // 发布过程中 MQTT 连接断开
    ConnectionLost,

    // 其他内部错误
    InternalError
};


// ==================================================
// MQTT 消息发布结果
// ==================================================

struct MqttPublishResult
{
    // 发布状态
    MqttPublishStatus status =
        MqttPublishStatus::InternalError;

    // 错误信息
    std::string error_message;


    // 判断消息是否收到 PUBACK
    [[nodiscard]]
    bool isAcknowledged() const noexcept
    {
        return status == MqttPublishStatus::Acknowledged;
    }
};


// ==================================================
// MQTT 接收消息
// ==================================================

struct MqttMessage
{
    // 消息所属 Topic
    std::string topic;

    // 消息内容
    std::string payload;

    // 实际接收的 QoS 等级
    int qos = 0;

    // 是否为 Broker 下发的保留消息
    bool retained = false;
};


// ==================================================
// MQTT 消息接收回调
// ==================================================
//
// 当 MQTT 客户端收到订阅消息时调用。
//
// 注意：
//
// 1. 回调由 MQTT 网络循环线程执行。
// 2. 消息对象只保证在当前回调执行期间有效。
// 3. 如果需要异步处理，必须复制消息内容。
// 4. 回调不应该执行耗时操作。
// 5. 建议将消息投递到应用层队列。
//
// ==================================================

using MqttMessageHandler =
    std::function<void(const MqttMessage&)>;


// ==================================================
// MQTT 订阅状态
// ==================================================

enum class MqttSubscriptionStatus
{
    // Broker 已通过 SUBACK 确认订阅
    Acknowledged,

    // Broker 拒绝订阅
    Rejected,

    // 向 MQTT 客户端提交订阅请求失败
    RequestFailed
};


// ==================================================
// MQTT 订阅结果
// ==================================================

struct MqttSubscriptionResult
{
    // 订阅状态
    MqttSubscriptionStatus status =
        MqttSubscriptionStatus::RequestFailed;

    // Broker 实际授予的 QoS
    //
    // -1 表示没有获得有效的 QoS 授权
    int granted_qos = -1;

    // 错误信息
    std::string error_message;


    // 判断是否成功订阅
    [[nodiscard]]
    bool isAcknowledged() const noexcept
    {
        return status == MqttSubscriptionStatus::Acknowledged;
    }
};


// ==================================================
// MQTT 订阅结果回调
// ==================================================
//
// 在 Broker 返回 SUBACK，或者订阅请求提交失败时
// 通知调用方。
//
// 自动重连后重新订阅，也会产生新的订阅结果。
//
// ==================================================

using MqttSubscriptionResultHandler =
    std::function<void(const MqttSubscriptionResult&)>;


// ==================================================
// MQTT 客户端抽象接口
// ==================================================
//
// 核心职责：
//
// 1. 启动 MQTT 客户端。
// 2. 管理 MQTT 网络连接。
// 3. 管理 MQTT 自动重连。
// 4. 发布 MQTT 消息。
// 5. 等待 QoS 1 的 PUBACK 确认。
// 6. 订阅 MQTT Topic。
// 7. 接收 Broker 下发的 MQTT 消息。
// 8. 管理断线重连后的自动重新订阅。
//
// 连接管理：
//
// 1. start() 启动 MQTT 网络循环。
// 2. 不要求 Broker 在启动时一定可用。
// 3. 连接失败后由具体实现负责自动重连。
// 4. stop() 停止 MQTT 客户端。
//
// 发布消息：
//
// 1. 使用 QoS 1。
// 2. 不启用 Retain。
// 3. 等待 PUBACK。
// 4. 超时或失败时返回对应状态。
//
// 订阅消息：
//
// 1. 支持 QoS 0、QoS 1、QoS 2。
// 2. 支持 MQTT Topic Filter。
// 3. 通过回调接收订阅消息。
// 4. 支持在 Broker 未连接时提前登记订阅。
// 5. Broker 连接成功后执行实际订阅。
// 6. Broker 重连后自动重新订阅。
// 7. 通过 SUBACK 结果回调通知订阅状态。
//
// 线程模型：
//
// 1. MQTT 网络循环由具体实现管理。
// 2. MQTT 消息回调由网络循环线程执行。
// 3. publish() 由 MqttPublisher 线程调用。
// 4. publish() 通过内部同步机制等待 PUBACK。
// 5. 不允许在 MQTT 网络回调线程中调用阻塞式 publish()。
//
// ==================================================

class IMqttClient
{
public:

    // ==================================================
    // 析构函数
    // ==================================================

    virtual ~IMqttClient() = default;


    // ==================================================
    // 一、MQTT 客户端生命周期
    // ==================================================

    // 启动 MQTT 客户端。
    //
    // 启动 MQTT 网络循环并发起 Broker 连接。
    //
    // 注意：
    //
    // true 表示客户端成功启动，
    // 不代表 Broker 已经连接成功。
    //
    // 即使 Broker 暂时不可用，也应该允许启动，
    // 后续由具体实现负责自动重连。
    //
    // error_message:
    // 启动失败时返回错误信息。
    //
    // 返回值：
    // true  - MQTT 客户端启动成功
    // false - MQTT 客户端启动失败

    virtual bool start(
        std::string& error_message
    ) = 0;


    // 停止 MQTT 客户端。
    //
    // 停止网络循环并释放 MQTT 连接资源。
    //
    // 应支持重复调用。

    virtual void stop() noexcept = 0;


    // ==================================================
    // 二、MQTT 连接状态
    // ==================================================

    // 查询当前是否已经连接到 MQTT Broker。

    virtual bool isConnected() const noexcept = 0;


    // ==================================================
    // 三、MQTT 消息发布
    // ==================================================

    // 发布一条 MQTT 消息。
    //
    // topic:
    // MQTT Topic。
    //
    // payload:
    // MQTT 消息内容。
    //
    // acknowledgement_timeout:
    // 等待 PUBACK 的最大时间。
    //
    // 固定发布策略：
    //
    // QoS    = 1
    // Retain = false
    //
    // 返回值：
    //
    // Acknowledged
    //     已收到 PUBACK。
    //
    // NotConnected
    //     当前未连接。
    //
    // PublishFailed
    //     消息提交失败。
    //
    // AcknowledgementTimeout
    //     等待 PUBACK 超时。
    //
    // ConnectionLost
    //     发布过程中连接断开。
    //
    // InternalError
    //     其他内部错误。
    //
    // 注意：
    //
    // 此函数可能阻塞调用线程，直到收到 PUBACK
    // 或达到 acknowledgement_timeout。
    //
    // 不允许从 MQTT 网络回调线程调用。

    virtual MqttPublishResult publish(
        const std::string& topic,
        const std::string& payload,
        std::chrono::milliseconds acknowledgement_timeout
    ) = 0;


    // ==================================================
    // 四、MQTT 消息订阅
    // ==================================================

    // 注册 MQTT Topic 订阅。
    //
    // topic_filter:
    // MQTT Topic Filter。
    //
    // 例如：
    //
    // gateway/config
    // gateway/+/config
    // gateway/# 
    //
    // qos:
    // 请求订阅的 QoS 等级。
    //
    // 允许的范围：
    // 0、1、2。
    //
    // message_handler:
    // 收到订阅消息时执行的回调。
    //
    // result_handler:
    // Broker 返回 SUBACK 或订阅请求提交失败时
    // 执行的结果回调。
    //
    // error_message:
    // 本地订阅登记失败时返回错误信息。
    //
    // 返回值：
    //
    // true:
    // 本地订阅登记成功。
    //
    // false:
    // 本地订阅登记失败，例如 Topic 不合法、
    // QoS 越界或回调为空。
    //
    // 重要说明：
    //
    // 1. 本函数可以在 MQTT 客户端启动之前调用。
    //
    // 2. 本函数返回 true 不代表 Broker 已经订阅成功。
    //
    // 3. 实际订阅成功需要等待 Broker 返回 SUBACK。
    //
    // 4. Broker 未连接时，具体实现应保存订阅意图，
    //    等待连接恢复后执行订阅。
    //
    // 5. Broker 重连后，具体实现应自动重新订阅。
    //
    // 6. 如果重复注册相同 Topic Filter，
    //    应以最新注册的 QoS 和消息回调为准。
    //
    // 7. 订阅结果通过 result_handler 通知调用方。
    //
    // 8. result_handler 为空时，具体实现仍应记录
    //    订阅失败等重要错误。
    //
    // 线程要求：
    //
    // message_handler 和 result_handler 均由 MQTT
    // 网络循环线程执行。
    //
    // 回调应该尽快返回，不应该执行耗时业务操作。
    //
    // ==================================================

    virtual bool subscribe(
        const std::string& topic_filter,
        int qos,
        MqttMessageHandler message_handler,
        MqttSubscriptionResultHandler result_handler,
        std::string& error_message
    ) = 0;

};

} // namespace industrial_gateway::application::ports
