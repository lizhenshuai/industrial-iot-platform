#pragma once

#include "industrial_gateway/application/ports/IDataQueue.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/config/ConnectionDefinition.hpp"
#include "industrial_gateway/config/CollectionConnectionConfig.hpp"
#include "ConnectionWorker.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


namespace industrial_gateway::application::connector
{
class ConnectorFactoryRegistry;
class ConnectionWorker;

// 对外返回值副本，不暴露 Worker 或协议驱动的可变引用。
// 两个布尔值分别采样，不保证是同一个瞬间的原子快照。
struct ConnectionStatus
{
    std::string connection_id;
    std::string protocol;
    bool running{false};
    bool connected{false};
    std::string endpoint_key;
};

// ==================================================
// 多连接运行管理器
// ==================================================
//
// 使用工厂返回的驱动和任务创建 ConnectionWorker，不理解协议寻址、
// 配置文件内容、寄存器或 NodeId，不执行协议连接和采集操作。
//
// 所有方法（包括状态查询）由同一个上层控制线程串行调用，不支持重入。
// MQTT 网络回调不得直接调用本对象；通过配置队列交给 ConfigManager 控制线程。
// Worker 内部状态通过其线程安全查询接口获取。
//
// 注册表、DataQueue、ILogger 必须比管理器及其 Worker 存活更久。
// 全局退出（包括启动失败后的清理）必须先关闭 DataQueue，再 stopAll()。
// 管理器不会自行关闭共享队列，也不停止 StorageWorker 或 MQTT 客户端。
//
// ==================================================

class ConnectorManager final
{
public:
    ConnectorManager(
        ConnectorFactoryRegistry& registry,
        ports::IDataQueue& data_queue,
        common::ILogger& logger
    );
    // 调用 stopAll()；调用方仍须遵守先关闭共享队列的退出约定。
    ~ConnectorManager();

    ConnectorManager(const ConnectorManager&) = delete;
    ConnectorManager& operator=(const ConnectorManager&) = delete;
    ConnectorManager(ConnectorManager&&) = delete;
    ConnectorManager& operator=(ConnectorManager&&) = delete;

    // 按一条静态连接定义创建并启动 Worker。
    // config_file 由上层事先按主配置文件目录解析为绝对路径。
    // 拒绝重复 connection_id 或同协议端点，即使旧 Worker 已因异常退出。
    //
    // true：Worker 线程已启动，不代表设备在线，也不保证线程随后不会退出。
    // false：当前候选实例被清理，已有连接保持原状，error_message 描述原因。
    // 成功清空 error_message。配置/工厂/构造异常转换为失败；错误信息自身
    // 的分配仍可能抛异常，因此本接口不是 noexcept。
    //
    // 不提供一批配置的自动回滚；批量启动中途失败时，由上层决定继续其他
    // 连接，或先关闭队列再 stopAll()，避免对阻塞入队的 Worker 盲目 join。
    [[nodiscard]]
    bool addAndStart(
        const config::ConnectionDefinition& definition,
        std::string& error_message
    );

    // 内存入口复用工厂；prepare 完成驱动、任务及 Worker 构造，不启动线程。
    // 失败保留 prepared 原值；传入的 prepared 必须为空或尚未启动的候选。
    // 候选应在同一控制线程中创建、应用或销毁。
    bool prepare(const config::CollectionConnectionConfig& definition,
                 std::unique_ptr<ConnectionWorker>& prepared, std::string& error_message);
    bool addAndStart(const config::CollectionConnectionConfig& definition, std::string& error_message);

    // 消费尚未启动的候选。新增先分配管理节点再启动，替换先准备候选再停止旧实例。
    // 替换启动失败时旧实例保留在管理器中但已停止，不承诺回滚，调用方须重新核对。
    // 不接受已运行/已连接的候选；候选必须由本管理器的 prepare() 创建。
    bool addPrepared(std::unique_ptr<ConnectionWorker> prepared, std::string& error_message);
    bool replacePrepared(std::unique_ptr<ConnectionWorker> prepared, std::string& error_message);
    bool remove(const std::string& connection_id, std::string& error_message);

    // 停止并回收所有 Worker，清空实例集合，可重复调用。
    // 会等待当前同步协议调用和阻塞入队返回；不承诺固定退出时限。
    // 队列持续满时需上层先 close()，仅调用本方法无法唤醒队列的 push()。
    // 单实例删除/替换也受相同阻塞限制，不能为此关闭所有连接共享的队列。
    void stopAll() noexcept;

    // 未登记的连接返回 nullopt；已异常退出但尚未回收的实例仍可查询。
    [[nodiscard]]
    std::optional<ConnectionStatus> status(const std::string& connection_id) const;

    // 按 connection_id 升序返回状态副本，便于稳定展示及记录。
    [[nodiscard]]
    std::vector<ConnectionStatus> statuses() const;

    // 已管理的实例数，包括已异常退出但尚未 stopAll() 回收的实例。
    [[nodiscard]]
    std::size_t size() const noexcept;

private:
    // 同一协议端点只允许一个已登记实例；替换时排除相同 connection_id 的旧实例。
    bool checkEndpoint(const ConnectionWorker& candidate, std::string& error_message) const;

    ConnectorFactoryRegistry& registry_;
    ports::IDataQueue& data_queue_;
    common::ILogger& logger_;
    std::unordered_map<std::string, std::unique_ptr<ConnectionWorker>> workers_;
};

} // namespace industrial_gateway::application::connector
