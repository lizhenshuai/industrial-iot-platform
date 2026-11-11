#include "ConnectorConfigApplier.hpp"

#include <nlohmann/json.hpp>
#include <exception>
#include <utility>

namespace industrial_gateway::application::configuration
{
ConnectorConfigApplier::ConnectorConfigApplier(connector::ConnectorManager& manager) : manager_(manager) {}

bool ConnectorConfigApplier::prepare(const ::industrial_gateway::config::CollectionConfigSnapshot& snapshot,
                                    std::string& error_message)
{
    error_message.clear();
    discard();
    try
    {
        auto candidate = std::make_unique<Pending>();
        std::map<std::pair<std::string, std::string>, std::string> endpoints;
        for (const auto& definition : snapshot.connections)
        {
            std::unique_ptr<connector::ConnectionWorker> worker;
            if (!manager_.prepare(definition, worker, error_message)) return false;
            // 以驱动解析后的端点为准，公共层无需识别不同协议的 JSON 字段或默认端口。
            const auto endpoint = std::make_pair(worker->protocolType(), worker->endpointKey());
            const auto entry = endpoints.emplace(endpoint, definition.connection_id);
            if (!entry.second)
            {
                error_message = "连接 '" + definition.connection_id + "' 与连接 '" +
                    entry.first->second + "' 使用相同协议端点: " + endpoint.first + "/" +
                    endpoint.second + "，请将设备配置放入同一连接";
                return false;
            }
            const auto fingerprint = std::make_pair(definition.protocol,
                nlohmann::json::parse(definition.config_json).dump());
            if (!candidate->target.emplace(definition.connection_id, fingerprint).second)
            {
                error_message = "配置中的连接 ID 重复";
                return false;
            }
            const auto old = applied_.find(definition.connection_id);
            const auto running = manager_.status(definition.connection_id);
            // 内容相同但 Worker 已退出时也必须重建，不能把“配置相同”当作“正在运行”。
            if (old == applied_.end() || old->second != fingerprint || !running ||
                !running->running || running->protocol != definition.protocol ||
                running->endpoint_key != worker->endpointKey())
                candidate->workers.push_back(std::move(worker));
        }
        for (const auto& current : manager_.statuses())
            if (candidate->target.find(current.connection_id) == candidate->target.end())
                candidate->removals.push_back(current.connection_id);
        pending_ = std::move(candidate);
        return true;
    }
    catch (const std::exception& error) { error_message = error.what(); }
    catch (...) { error_message = "准备配置更新时发生未知异常"; }
    return false;
}

bool ConnectorConfigApplier::apply(std::string& error_message)
{
    error_message.clear();
    if (!pending_) { error_message = "没有已准备的配置更新"; return false; }
    auto update = std::move(pending_);
    // 执行前清空可信比较基线。任一操作失败时保留空基线，重试会重建目标连接。
    applied_.clear();
    try
    {
        for (const auto& id : update->removals)
            if (!manager_.remove(id, error_message)) return false;
        // 变化的旧连接全部退出后才能启动新连接。若 A/B 交换 IP:端口，逐个替换
        // 会与尚未退出的旧实例冲突。此处只移除变化的连接，未变化线程持续运行。
        for (const auto& worker : update->workers)
            if (!manager_.remove(worker->connectionId(), error_message)) return false;
        for (auto& worker : update->workers)
            if (!manager_.addPrepared(std::move(worker), error_message)) return false;
        applied_.swap(update->target);
        return true;
    }
    catch (const std::exception& error) { error_message = error.what(); }
    catch (...) { error_message = "应用配置时发生未知异常，需重新核对运行连接"; }
    return false;
}

void ConnectorConfigApplier::discard() noexcept { pending_.reset(); }

bool ConnectorConfigApplier::checkRunningConnections(std::string& error_message) const
{
    error_message.clear();
    for (const auto& entry : applied_)
    {
        const auto state = manager_.status(entry.first);
        if (!state || !state->running)
        {
            // Worker 当前只提供存活状态；底层异常详情仍在 Worker 日志中。
            error_message = "采集线程异常退出或缺失，connection_id=" + entry.first +
                "，protocol=" + entry.second.first + "；具体采集错误请查看 ConnectionWorker 日志";
            return false;
        }
    }
    return true;
}
} // namespace industrial_gateway::application::configuration
