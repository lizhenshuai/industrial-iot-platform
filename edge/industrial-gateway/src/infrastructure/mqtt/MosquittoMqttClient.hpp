
#pragma once

#include "industrial_gateway/application/ports/IMqttClient.hpp"

#include "industrial_gateway/common/ILogger.hpp"

#include "industrial_gateway/config/MqttConfig.hpp"

#include <chrono>
#include <memory>
#include <string>


namespace industrial_gateway::infrastructure::mqtt
{

// ==================================================
// Mosquitto MQTT 客户端
// ==================================================
//
// 实现 application::ports::IMqttClient 接口。
//
// 核心职责：
//
// 1. 管理 Mosquitto 客户端生命周期。
// 2. 建立 MQTT Broker 连接。
// 3. 管理 MQTT 网络循环。
// 4. 处理 MQTT 自动重连。
// 5. 发布 QoS 1 消息。
// 6. 接收 PUBACK 确认。
// 7. 订阅 MQTT Topic。
// 8. 接收 Broker 下发的消息。
// 9. 处理 SUBACK 订阅确认。
// 10. 断线重连后自动恢复订阅。
// 11. 向应用层返回统一的 MQTT 操作结果。
//
// 线程模型：
//
// 1. MQTT 网络循环由 Mosquitto 内部线程执行。
// 2. MQTT 回调由网络循环线程执行。
// 3. publish() 由唯一的 MqttPublisher 线程调用。
// 4. publish() 通过内部同步机制等待 PUBACK。
// 5. 不允许多个业务线程并发调用 publish()。
// 6. subscribe() 负责登记订阅意图。
// 7. 实际订阅由 MQTT 网络循环处理。
// 8. 不允许在 MQTT 回调线程中调用阻塞式 publish()。
// 9. 不允许在 MQTT 回调线程中调用 stop()。
//
// 设计原则：
//
// 1. 使用 PImpl 隐藏 Mosquitto 具体实现。
// 2. 不向应用层暴露 Mosquitto 类型。
// 3. 一个 MosquittoMqttClient 管理一个 MQTT 客户端实例。
// 4. MQTT 连接断开不影响本地 SQLite 数据缓存。
// 5. MQTT 连接恢复后自动重新订阅已经登记的 Topic。
// 6. 配置对象由客户端内部保存副本。
// 7. 日志对象由外部管理，生命周期必须长于当前客户端。
//
// 生命周期安全要求：
//
// 1. 析构函数主动调用 stop()。
// 2. stop() 必须支持重复调用。
// 3. stop() 必须唤醒正在等待 PUBACK 的发布线程。
// 4. stop() 返回前必须确保 MQTT 网络循环已经停止。
// 5. stop() 返回前必须确保正在执行的 MQTT 回调已经结束。
// 6. stop() 返回前必须确保正在执行的 publish() 已退出内部等待。
// 7. 不允许网络线程访问已经销毁的 Impl。
//
// 回调安全要求：
//
// 1. 所有外部回调均由 MQTT 网络线程执行。
// 2. 回调异常必须由具体实现捕获。
// 3. 不允许 C++ 异常穿透 Mosquitto 的 C 回调边界。
// 4. 调用外部回调时，不允许持有内部订阅管理锁。
// 5. 回调不应该执行耗时业务操作。
// 6. 建议将接收消息投递到应用层队列。
//
// ==================================================

class MosquittoMqttClient final
    : public ::industrial_gateway::application::ports::IMqttClient
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    // 构造 Mosquitto MQTT 客户端。
    //
    // config:
    // MQTT 配置。
    //
    // logger:
    // 日志接口。
    //
    // 注意：
    //
    // 1. config 在内部保存副本，不依赖外部配置对象生命周期。
    // 2. logger 保留引用，生命周期必须长于当前 MQTT 客户端。
    // 3. 构造阶段不建立 MQTT 网络连接。
    //
    // ==================================================

    MosquittoMqttClient(
        const ::industrial_gateway::config::MqttConfig& config,
        ::industrial_gateway::common::ILogger& logger
    );


    // ==================================================
    // 析构函数
    // ==================================================

    // 析构时主动停止 MQTT 客户端。
    //
    // 具体实现必须保证：
    //
    // 1. stop() 可以重复调用。
    // 2. MQTT 网络线程已经安全退出。
    // 3. 正在执行的 MQTT 回调已经结束。
    // 4. 正在等待 PUBACK 的发布操作已经退出。
    // 5. 不会继续访问已经释放的 Mosquitto 资源。
    //
    // 注意：
    // 析构函数不能从 MQTT 网络回调线程中触发。

    ~MosquittoMqttClient() noexcept override;


    // ==================================================
    // 禁止复制和移动
    // ==================================================

    MosquittoMqttClient(
        const MosquittoMqttClient&
    ) = delete;

    MosquittoMqttClient& operator=(
        const MosquittoMqttClient&
    ) = delete;

    MosquittoMqttClient(
        MosquittoMqttClient&&
    ) = delete;

    MosquittoMqttClient& operator=(
        MosquittoMqttClient&&
    ) = delete;


    // ==================================================
    // 一、MQTT 客户端生命周期
    // ==================================================

    // 启动 MQTT 客户端。
    //
    // 启动 Mosquitto 网络循环并发起 Broker 连接。
    //
    // 注意：
    //
    // 1. true 表示客户端成功启动，
    //    不代表 Broker 已经连接成功。
    //
    // 2. Broker 暂时不可用不应该直接导致
    //    MQTT 客户端启动失败。
    //
    // 3. 连接失败后由具体实现负责自动重连。
    //
    // 4. 启动前可以提前登记固定 Topic 订阅。
    //
    // error_message:
    // 启动失败时返回错误信息。
    //
    // 返回值：
    // true  - 客户端启动成功
    // false - 客户端启动失败
    //
    // ==================================================

    [[nodiscard]]
    bool start(
        std::string& error_message
    ) override;


    // 停止 MQTT 客户端。
    //
    // 停止网络循环并释放相关资源。
    //
    // 必须保证：
    //
    // 1. 支持重复调用。
    // 2. 唤醒正在等待 PUBACK 的发布线程。
    // 3. 等待网络循环线程安全退出。
    // 4. 等待正在执行的 MQTT 回调结束。
    // 5. 不会发生资源释放与线程访问之间的竞争。
    //
    // 注意：
    //
    // 1. 不允许从 MQTT 网络回调线程调用。
    // 2. 应先停止 MqttPublisher，再停止 MQTT 客户端。
    //
    // ==================================================

    void stop() noexcept override;


    // ==================================================
    // 二、MQTT 连接状态
    // ==================================================

    // 查询当前是否已经连接到 MQTT Broker。
    //
    // 线程安全：
    // 允许其他业务线程调用。
    //
    // 返回值：
    // true  - 当前已经连接
    // false - 当前未连接
    //
    // ==================================================

    [[nodiscard]]
    bool isConnected() const noexcept override;

    // 启动装配层的线程健康查询，与 Broker 是否在线分离。
    // 连接中/退避重连时为 true，网络线程终止或不可恢复故障时为 false。
    // 本接口不扩展业务端口 IMqttClient，业务发布/订阅仍只依赖原接口。
    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] std::string failureReason() const;


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
    // 固定发布参数：
    //
    // QoS    = 1
    // Retain = false
    //
    // 返回值：
    //
    // Acknowledged
    //     已收到对应消息的 PUBACK。
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
    //     发布过程中 MQTT 连接断开。
    //
    // InternalError
    //     其他内部错误。
    //
    // 注意：
    //
    // 1. 此函数可能阻塞调用线程，直到收到 PUBACK
    //    或达到 acknowledgement_timeout。
    //
    // 2. 只有收到 PUBACK 才返回 Acknowledged。
    //
    // 3. 超时不代表 Broker 一定没有收到消息。
    //
    // 4. 超时或失败时，上层不得删除 SQLite 中的消息。
    //
    // 5. 可能出现重复发送，需要依靠 message_id
    //    实现业务层幂等处理。
    //
    // 6. 仅允许唯一的 MqttPublisher 线程串行调用。
    //
    // 7. 不允许从 MQTT 网络回调线程调用。
    //
    // 8. stop() 必须能够唤醒正在等待的发布操作。
    //
    // ==================================================

    [[nodiscard]]
    ::industrial_gateway::application::ports::MqttPublishResult publish(
        const std::string& topic,
        const std::string& payload,
        std::chrono::milliseconds acknowledgement_timeout
    ) override;


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
    // 订阅行为：
    //
    // 1. 可以在 MQTT 客户端启动之前登记订阅。
    //
    // 2. Broker 未连接时保存订阅意图。
    //
    // 3. Broker 连接成功后执行实际订阅。
    //
    // 4. Broker 重连后自动重新订阅。
    //
    // 5. 实际订阅结果通过 result_handler 通知。
    //
    // 6. 重复注册相同 Topic Filter 时，
    //    以最新注册的 QoS 和消息回调为准。
    //
    // 7. result_handler 为空时，
    //    具体实现仍应记录重要订阅错误。
    //
    // 线程安全：
    //
    // 1. 订阅登记需要与 MQTT 网络线程安全同步。
    //
    // 2. 内部订阅管理锁不能跨越外部回调执行。
    //
    // 3. 实际 SUBSCRIBE 操作由 MQTT 网络线程处理。
    //
    // 回调要求：
    //
    // 1. message_handler 由 MQTT 网络线程执行。
    // 2. result_handler 由 MQTT 网络线程执行。
    // 3. 回调异常必须由具体实现捕获。
    // 4. 不允许异常传播到 Mosquitto 的 C 回调边界。
    // 5. 回调不应该执行耗时业务操作。
    //
    // ==================================================

    [[nodiscard]]
    bool subscribe(
        const std::string& topic_filter,
        int qos,
        ::industrial_gateway::application::ports::MqttMessageHandler message_handler,
        ::industrial_gateway::application::ports::MqttSubscriptionResultHandler result_handler,
        std::string& error_message
    ) override;


private:

    // ==================================================
    // PImpl：隐藏 Mosquitto 具体实现
    // ==================================================

    class Impl;

    std::unique_ptr<Impl> impl_;

};

} // namespace industrial_gateway::infrastructure::mqtt
