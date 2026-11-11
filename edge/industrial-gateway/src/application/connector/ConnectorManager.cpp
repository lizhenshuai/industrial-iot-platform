#include "ConnectorManager.hpp"

#include "ConnectionWorker.hpp"
#include "ConnectorFactoryRegistry.hpp"

#include <algorithm>
#include <exception>
#include <utility>


namespace industrial_gateway::application::connector
{
namespace
{

ConnectionStatus snapshot(const ConnectionWorker& worker)
{
    return {worker.connectionId(), worker.protocolType(),
            worker.isRunning(), worker.isConnected(), worker.endpointKey()};
}

} // namespace


ConnectorManager::ConnectorManager(
    ConnectorFactoryRegistry& registry,
    ports::IDataQueue& data_queue,
    common::ILogger& logger
)
    : registry_(registry), data_queue_(data_queue), logger_(logger)
{
}

ConnectorManager::~ConnectorManager()
{
    stopAll();
}

bool ConnectorManager::addAndStart(
    const config::ConnectionDefinition& definition,
    std::string& error_message
)
{
    error_message.clear();
    // 只有当前调用成功插入的实例才可以在失败时移除，不能删除同 ID 的旧实例。
    auto inserted = workers_.end();
    const auto discardCandidate = [&]()
    {
        if (inserted != workers_.end())
        {
            // start() 失败时尚无运行线程；stop() 也支持未启动状态。
            inserted->second->stop();
            workers_.erase(inserted);
            inserted = workers_.end();
        }
    };
    const auto fail = [&](const std::string& reason)
    {
        error_message = "启动连接 '" + definition.connection_id + "' 失败: " + reason;
        return false;
    };

    try
    {
        if (workers_.find(definition.connection_id) != workers_.end())
            return fail("connection_id 已存在，不允许覆盖现有实例");
        if (data_queue_.isClosed()) return fail("DataQueue 已关闭");

        ConnectorCreationResult created;
        if (!registry_.create(definition, logger_, created, error_message)) return false;

        auto worker = std::make_unique<ConnectionWorker>(
            std::move(created.connector), std::move(created.tasks), data_queue_, logger_);
        if (!checkEndpoint(*worker, error_message)) return false;

        // 必须先完成容器分配，再启动线程。
        // 如果先启动后插入，插入分配失败时销毁正在 push() 的 Worker 可能卡住。
        const auto entry = workers_.try_emplace(definition.connection_id, std::move(worker));
        if (!entry.second) return fail("connection_id 已存在");
        inserted = entry.first;

        std::string start_error;
        if (!inserted->second->start(start_error))
        {
            discardCandidate();
            return fail(start_error.empty() ? "Worker 启动失败但未提供错误原因" : start_error);
        }
        // 线程启动成功之后不再做可抛异常的分配、日志或配置操作。
        return true;
    }
    catch (const std::exception& error)
    {
        discardCandidate();
        return fail(std::string("创建或启动过程发生异常: ") + error.what());
    }
    catch (...)
    {
        discardCandidate();
        return fail("创建或启动过程发生未知异常");
    }
}

bool ConnectorManager::prepare(const config::CollectionConnectionConfig& definition,
                               std::unique_ptr<ConnectionWorker>& prepared, std::string& error_message)
{
    error_message.clear();
    try
    {
        ConnectorCreationResult created;
        if (!registry_.create(definition, logger_, created, error_message)) return false;
        auto candidate = std::make_unique<ConnectionWorker>(std::move(created.connector),
            std::move(created.tasks), data_queue_, logger_);
        prepared = std::move(candidate);
        return true;
    }
    catch (const std::exception& error) { error_message = error.what(); }
    catch (...) { error_message = "准备连接候选时发生未知异常"; }
    return false;
}

bool ConnectorManager::addAndStart(const config::CollectionConnectionConfig& definition,
                                   std::string& error_message)
{
    std::unique_ptr<ConnectionWorker> prepared;
    return prepare(definition, prepared, error_message) && addPrepared(std::move(prepared), error_message);
}

bool ConnectorManager::addPrepared(std::unique_ptr<ConnectionWorker> prepared, std::string& error_message)
{
    error_message.clear();
    auto inserted = workers_.end();
    try
    {
        if (!prepared || prepared->isRunning() || prepared->isConnected())
        {
            error_message = "新增连接需要尚未运行的有效候选";
            return false;
        }
        if (data_queue_.isClosed()) { error_message = "DataQueue 已关闭"; return false; }
        if (!checkEndpoint(*prepared, error_message)) return false;
        const auto id = prepared->connectionId();
        const auto entry = workers_.try_emplace(id, std::move(prepared));
        if (!entry.second) { error_message = "连接 ID 已存在: " + id; return false; }
        inserted = entry.first;
        if (inserted->second->start(error_message)) return true;
    }
    catch (const std::exception& error) { error_message = error.what(); }
    catch (...) { error_message = "新增连接时发生未知异常"; }
    if (inserted != workers_.end()) workers_.erase(inserted);
    if (error_message.empty()) error_message = "新增连接启动失败";
    return false;
}

bool ConnectorManager::replacePrepared(std::unique_ptr<ConnectionWorker> prepared, std::string& error_message)
{
    error_message.clear();
    try
    {
        if (!prepared || prepared->isRunning() || prepared->isConnected())
        {
            error_message = "替换连接需要尚未运行的有效候选";
            return false;
        }
        if (data_queue_.isClosed()) { error_message = "DataQueue 已关闭"; return false; }
        const auto found = workers_.find(prepared->connectionId());
        if (found == workers_.end()) { error_message = "待替换连接不存在"; return false; }
        // 先检查冲突，非法替换不能停掉仍然有效的旧线程。
        if (!checkEndpoint(*prepared, error_message)) return false;
        // 不修改 Worker 的停止机制；队列持续满时此处仍可能等待，保留已知限制。
        found->second->stop();
        if (!prepared->start(error_message))
        {
            if (error_message.empty()) error_message = "替换连接启动失败，旧连接已停止";
            return false;
        }
        // 启动成功后只做 noexcept 所有权交换，旧 Worker 此时已停止。
        found->second.swap(prepared);
        return true;
    }
    catch (const std::exception& error) { error_message = error.what(); }
    catch (...) { error_message = "替换连接时发生未知异常"; }
    return false;
}

bool ConnectorManager::remove(const std::string& connection_id, std::string& error_message)
{
    error_message.clear();
    const auto found = workers_.find(connection_id);
    if (found == workers_.end()) return true; // 删除不存在的连接视为已达成目标。
    found->second->stop();
    workers_.erase(found);
    return true;
}

bool ConnectorManager::checkEndpoint(const ConnectionWorker& candidate, std::string& error_message) const
{
    for (const auto& entry : workers_)
    {
        const auto& current = *entry.second;
        if (current.connectionId() != candidate.connectionId() &&
            current.protocolType() == candidate.protocolType() &&
            current.endpointKey() == candidate.endpointKey())
        {
            error_message = "连接 '" + candidate.connectionId() + "' 与连接 '" +
                current.connectionId() + "' 使用相同协议端点: " + candidate.protocolType() +
                "/" + candidate.endpointKey() + "，请将设备配置放入同一连接";
            return false;
        }
    }
    return true;
}

void ConnectorManager::stopAll() noexcept
{
    // 上层已经关闭共享队列，阻塞的生产者可退出；协议操作由各 Worker
    // 自己收尾，管理器不跨线程调用驱动 disconnect()。
    for (auto& entry : workers_) entry.second->stop();
    workers_.clear();
}

std::optional<ConnectionStatus> ConnectorManager::status(const std::string& connection_id) const
{
    const auto found = workers_.find(connection_id);
    if (found == workers_.end()) return std::nullopt;
    return snapshot(*found->second);
}

std::vector<ConnectionStatus> ConnectorManager::statuses() const
{
    std::vector<ConnectionStatus> result;
    result.reserve(workers_.size());
    for (const auto& entry : workers_) result.push_back(snapshot(*entry.second));
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        return left.connection_id < right.connection_id;
    });
    return result;
}

std::size_t ConnectorManager::size() const noexcept
{
    return workers_.size();
}

} // namespace industrial_gateway::application::connector
