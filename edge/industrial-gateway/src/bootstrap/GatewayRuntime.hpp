#pragma once

#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/config/GatewayConfig.hpp"

#include <memory>
#include <string>

namespace industrial_gateway::bootstrap
{
// 装配 MQTT 配置控制、协议采集和遥测存储发布链路。
// 生命周期方法仅由主线程调用；logger 必须存活到本对象销毁后。
// 每个实例只启动一次，停止后共享队列不可重新打开。
class GatewayRuntime final
{
public:
    GatewayRuntime(const config::GatewayConfig& config, common::ILogger& logger);
    ~GatewayRuntime();
    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;
    GatewayRuntime(GatewayRuntime&&) = delete;
    GatewayRuntime& operator=(GatewayRuntime&&) = delete;

    // true 表示基础线程启动，配置恢复在后台进行，不表示设备或 MQTT 在线。
    bool start(std::string& error_message);
    // 先关闭共享采集队列再等待配置/采集线程；存储按原有规则排空。
    // SQLite 持续写入失败时，StorageWorker 仍会重试，因此不保证固定退出时限。
    void stop() noexcept;
    // 汇总关键线程存活/故障状态，不代表设备或 Broker 在线。
    // 采集状态由配置线程检查后报告，不从主线程并发遍历连接集合。
    bool isRunning() const noexcept;
    // 由主线程调用；健康时清空原因，故障时返回 false 并提供组件/连接信息。
    bool checkHealth(std::string& error_message) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace industrial_gateway::bootstrap
