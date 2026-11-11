#include "ConnectorFactoryRegistry.hpp"

#include <exception>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <utility>


namespace industrial_gateway::application::connector
{
namespace
{

bool validName(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos &&
           value.find('\0') == std::string::npos;
}

// 只检查创建结果的公共边界，不依赖具体协议，也不执行网络操作。
bool validateCreated(const std::string& connection_id, const std::string& protocol,
                     const ConnectorCreationResult& created,
                     std::string& error_message)
{
    const auto fail = [&](const std::string& reason)
    {
        error_message = reason;
        return false;
    };
    if (!created.connector) return fail("工厂返回了空驱动");
    if (created.connector->connectionId() != connection_id)
        return fail("工厂返回驱动的 connection_id 与连接定义不一致");
    if (created.connector->protocolType() != protocol)
        return fail("工厂返回驱动的 protocolType 与注册协议不一致");
    if (created.connector->isConnected())
        return fail("工厂不应建立设备连接，连接生命周期由 ConnectionWorker 管理");
    if (created.tasks.empty()) return fail("工厂未返回采集任务");

    std::unordered_set<std::chrono::milliseconds::rep> intervals;
    for (std::size_t i = 0; i < created.tasks.size(); ++i)
    {
        const auto& task = created.tasks[i];
        const auto path = "tasks[" + std::to_string(i) + "]: ";
        if (task.connection_id != connection_id)
            return fail(path + "任务不属于当前连接");
        if (task.interval.count() <= 0)
            return fail(path + "采集周期必须大于 0");
        if (!intervals.insert(task.interval.count()).second)
            return fail(path + "同一连接的频率任务重复");
        if (task.devices.empty()) return fail(path + "设备集合不能为空");
    }
    return true;
}

} // namespace


bool ConnectorFactoryRegistry::registerFactory(
    const std::string& protocol,
    Factory factory,
    std::string& error_message
)
{
    // 保留静态配置调用方的接口，空文件工厂仍会被统一入口拒绝。
    return registerFactory(protocol, std::move(factory), MemoryFactory{}, error_message);
}

bool ConnectorFactoryRegistry::registerFactory(
    const std::string& protocol,
    Factory file_factory,
    MemoryFactory memory_factory,
    std::string& error_message
)
{
    error_message.clear();
    if (!validName(protocol))
    {
        error_message = "协议名称不能为空、全空白或包含空字符";
        return false;
    }
    if (!file_factory && !memory_factory)
    {
        error_message = "协议 " + protocol + " 至少需要一个文件或内存配置工厂";
        return false;
    }

    try
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        // 两个入口以一个元素插入，重复键和插入失败均不修改已有注册项。
        if (!factories_.try_emplace(protocol,
                FactoryEntry{std::move(file_factory), std::move(memory_factory)}).second)
        {
            error_message = "协议工厂已注册，不允许覆盖: " + protocol;
            return false;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        error_message = "注册协议 " + protocol + " 失败: " + error.what();
    }
    catch (...)
    {
        error_message = "注册协议 " + protocol + " 时发生未知异常";
    }
    return false;
}


bool ConnectorFactoryRegistry::create(
    const config::ConnectionDefinition& definition,
    common::ILogger& logger,
    ConnectorCreationResult& result,
    std::string& error_message
) const
{
    error_message.clear();
    const auto fail = [&](const std::string& reason)
    {
        error_message = "创建连接 '" + definition.connection_id + "' (protocol=" +
                        definition.protocol + ") 失败: " + reason;
        return false;
    };

    try
    {
        if (!validName(definition.connection_id))
            return fail("connection_id 不能为空、全空白或包含空字符");
        if (!validName(definition.protocol))
            return fail("protocol 不能为空、全空白或包含空字符");
        if (!validName(definition.config_file))
            return fail("config_file 不能为空、全空白或包含空字符");
        if (!std::filesystem::path(definition.config_file).is_absolute())
            return fail("config_file 必须由上层按主配置目录解析为绝对路径");

        Factory factory;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            const auto found = factories_.find(definition.protocol);
            if (found == factories_.end()) return fail("未知协议，未注册对应工厂");
            factory = found->second.file_factory;
        }
        if (!factory) return fail("该协议未提供文件配置工厂");

        // 不持有注册表锁执行外部工厂；工厂可读取配置、构造驱动及任务。
        // 临时结果负责失败路径上的资源释放，调用方原有结果保持不变。
        ConnectorCreationResult candidate;
        std::string factory_error;
        if (!factory(definition, logger, candidate, factory_error))
            return fail(factory_error.empty() ? "工厂返回失败但未提供错误原因" : factory_error);
        if (!validateCreated(definition.connection_id, definition.protocol, candidate, factory_error))
            return fail(factory_error);

        result = std::move(candidate);
        return true;
    }
    catch (const std::exception& error)
    {
        return fail(std::string("创建过程发生异常: ") + error.what());
    }
    catch (...)
    {
        return fail("创建过程发生未知异常");
    }
}

bool ConnectorFactoryRegistry::create(
    const config::CollectionConnectionConfig& definition,
    common::ILogger& logger,
    ConnectorCreationResult& result,
    std::string& error_message
) const
{
    error_message.clear();
    const auto fail = [&](const std::string& reason)
    {
        error_message = "从内存配置创建连接 '" + definition.connection_id + "' (protocol=" +
                        definition.protocol + ") 失败: " + reason;
        return false;
    };

    try
    {
        if (!validName(definition.connection_id))
            return fail("connection_id 不能为空、全空白或包含空字符");
        if (!validName(definition.protocol))
            return fail("protocol 不能为空、全空白或包含空字符");
        if (!validName(definition.config_json))
            return fail("config_json 不能为空、全空白或包含原始空字符");

        MemoryFactory factory;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            const auto found = factories_.find(definition.protocol);
            if (found == factories_.end()) return fail("未知协议，未注册对应工厂");
            factory = found->second.memory_factory;
        }
        if (!factory) return fail("该协议未提供内存配置工厂");

        // 与静态入口一致：不持有注册表锁解析配置或构建实例。
        // 先完整创建并校验候选结果，成功前不动调用方已有的驱动和任务。
        ConnectorCreationResult candidate;
        std::string factory_error;
        if (!factory(definition, logger, candidate, factory_error))
            return fail(factory_error.empty() ? "工厂返回失败但未提供错误原因" : factory_error);
        if (!validateCreated(definition.connection_id, definition.protocol, candidate, factory_error))
            return fail(factory_error);

        result = std::move(candidate);
        return true;
    }
    catch (const std::exception& error)
    {
        return fail(std::string("创建过程发生异常: ") + error.what());
    }
    catch (...)
    {
        return fail("创建过程发生未知异常");
    }
}

} // namespace industrial_gateway::application::connector
