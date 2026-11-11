#pragma once

#include "industrial_gateway/application/ports/IProtocolConnector.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/config/CollectionConnectionConfig.hpp"
#include "industrial_gateway/config/ConnectionDefinition.hpp"
#include "industrial_gateway/domain/model/CollectionTask.hpp"

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>


namespace industrial_gateway::application::connector
{

// 一个连接实例的创建结果，所有权由调用方转交给 ConnectionWorker。
// 驱动与任务必须来自同一份已经校验的协议配置。
struct ConnectorCreationResult
{
    std::unique_ptr<ports::IProtocolConnector> connector;
    std::vector<domain::model::CollectionTask> tasks;
};


// ==================================================
// 内置协议工厂注册表
// ==================================================
//
// 将协议名称映射到文件 / 内存配置创建函数，不包含具体协议的条件分支。
// 协议实现编译进程序，启动时显式注册，运行时按配置创建多个实例。
// 不加载动态库，不连接设备、不创建 Worker、不操作 DataQueue。
//
// 推荐在创建连接实例前完成全部注册；不支持覆盖或注销已有工厂。
// 注册和查找受锁保护，调用工厂前释放锁，配置读取不占用注册表锁。
// 工厂捕获的外部依赖必须在调用期间有效；并发 create() 时，工厂及其
// 共享依赖须自行保证线程安全。注册表销毁时不得再有并发调用。
//
// ==================================================

class ConnectorFactoryRegistry final
{
public:
    // 工厂负责解析协议配置、校验、创建驱动，并按频率生成完整任务列表。
    // 工厂不得启动线程或建立设备连接，也不得保留 definition 的引用。
    // logger 可以被创建的驱动引用，因此必须比驱动和 Worker 存活更久。
    // true 表示创建成功，result 包含尚未连接的驱动及非空任务列表。
    // false 表示创建失败，error_message 应描述原因。
    using Factory = std::function<bool(
        const config::ConnectionDefinition& definition,
        common::ILogger& logger,
        ConnectorCreationResult& result,
        std::string& error_message
    )>;

    // 动态配置入口：直接解析 definition.config_json，不读取配置文件。
    // 工厂须独立校验 JSON 结构和协议语义，不能假设上层已经校验。
    // 其余生命周期、结果完整性和异常约定与文件 Factory 相同。
    using MemoryFactory = std::function<bool(
        const config::CollectionConnectionConfig& definition,
        common::ILogger& logger,
        ConnectorCreationResult& result,
        std::string& error_message
    )>;

    ConnectorFactoryRegistry() = default;
    ~ConnectorFactoryRegistry() = default;

    ConnectorFactoryRegistry(const ConnectorFactoryRegistry&) = delete;
    ConnectorFactoryRegistry& operator=(const ConnectorFactoryRegistry&) = delete;
    ConnectorFactoryRegistry(ConnectorFactoryRegistry&&) = delete;
    ConnectorFactoryRegistry& operator=(ConnectorFactoryRegistry&&) = delete;

    // 协议名按原字符串精确匹配，区分大小写，不自动转换或去除空白。
    // 拒绝空/全空白/含空字符的名称、空工厂及重复注册；不替换原工厂。
    // 成功清空 error_message，失败填写原因。
    [[nodiscard]]
    bool registerFactory(
        const std::string& protocol,
        Factory factory,
        std::string& error_message
    );

    // 一次登记同一协议的文件和内存入口，至少提供其中一个。
    // 两个入口作为一个注册项提交，不会出现部分注册成功的状态。
    // 同一 protocol 只能注册一次；不能用第二次调用补充或覆盖入口。
    // 上面的三参数入口保留原用法，仅注册文件工厂。
    [[nodiscard]]
    bool registerFactory(
        const std::string& protocol,
        Factory file_factory,
        MemoryFactory memory_factory,
        std::string& error_message
    );

    // 读取 definition.protocol 对应的工厂，在临时结果中完成创建。
    // 第一版 config_file 必须已由上层解析为非空绝对路径；本方法不读文件。
    // 检查返回驱动的连接 ID、协议类型、未连接状态和任务归属。
    // 协议寻址校验由工厂负责，任务完整校验在 ConnectionWorker 构造时执行。
    // 注册表不保存实例，不检查不同 create() 调用之间的 connection_id 冲突；
    // 运行实例的 ID 唯一性由 ConnectorManager 管理。
    //
    // true：更新 result，清空 error_message。
    // false：保留调用方原有 result，销毁临时结果，填写包含实例上下文的错误。
    // 工厂抛出的异常在此转换为创建失败，未知协议也返回 false。
    // 内存不足等情况下错误信息本身的构造仍可能抛异常，因此不是 noexcept。
    [[nodiscard]]
    bool create(
        const config::ConnectionDefinition& definition,
        common::ILogger& logger,
        ConnectorCreationResult& result,
        std::string& error_message
    ) const;

    // 根据内存连接配置创建尚未运行的驱动及任务。
    // 校验 connection_id、protocol 和非空且不含原始空字符的 config_json；
    // JSON 类型、语法及协议字段由对应 MemoryFactory 负责完整校验。
    // 未注册协议或该协议不支持内存配置时返回失败，不回退到文件入口。
    // 与文件入口共用创建结果校验；失败保留原 result，成功清空错误信息。
    // 不创建 Worker，不连接设备，也不保存配置或启动采集。
    [[nodiscard]]
    bool create(
        const config::CollectionConnectionConfig& definition,
        common::ILogger& logger,
        ConnectorCreationResult& result,
        std::string& error_message
    ) const;

private:
    struct FactoryEntry
    {
        Factory file_factory;
        MemoryFactory memory_factory;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FactoryEntry> factories_;
};

} // namespace industrial_gateway::application::connector
