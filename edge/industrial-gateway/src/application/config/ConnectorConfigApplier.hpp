#pragma once

#include "../connector/ConnectorManager.hpp"
#include "industrial_gateway/config/CollectionConfigSnapshot.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace industrial_gateway::application::configuration
{
// 单控制线程使用；prepare/apply 之间不得由其他调用方修改 ConnectorManager。
// 不持久化历史状态，只记录已成功应用的连接内容以跳过未变化的运行连接。
class ConnectorConfigApplier final
{
public:
    explicit ConnectorConfigApplier(connector::ConnectorManager& manager);
    // 全部连接均通过工厂、Worker 构造及协议端点唯一性校验后才提交准备结果。
    // 不改运行集合，重复端点在持久化之前拒绝。
    // 保守比较规范化 JSON：忽略对象键顺序，保留数组顺序及数字类型差异。
    bool prepare(const ::industrial_gateway::config::CollectionConfigSnapshot& snapshot,
                 std::string& error_message);
    // 仅应用最近一次成功准备的结果，成功或失败都消费该准备结果。
    // 先移除消失和变化的旧实例，再启动候选，允许两条连接交换端点。
    // 失败可能已改变部分连接，不自动回滚；下次 prepare 会重新核对全部连接。
    bool apply(std::string& error_message);
    void discard() noexcept;
    // 仅配置控制线程调用，在完整配置操作之间检查，不能与 prepare/apply 并发。
    // 按已应用目标核对，空目标合法；设备离线但 Worker 存活时仍为健康。
    bool checkRunningConnections(std::string& error_message) const;
private:
    using Fingerprints = std::map<std::string, std::pair<std::string, std::string>>;
    struct Pending
    {
        Fingerprints target;
        std::vector<std::string> removals;
        std::vector<std::unique_ptr<connector::ConnectionWorker>> workers;
    };
    connector::ConnectorManager& manager_;
    Fingerprints applied_;
    std::unique_ptr<Pending> pending_;
};
} // namespace industrial_gateway::application::configuration
