#pragma once

#include <string>
#include <vector>


namespace industrial_gateway::domain::model
{

// 后续分别在 CollectionTask.hpp 和 CollectionResult.hpp 中定义。
struct CollectionTask;
struct CollectionResult;

} // namespace industrial_gateway::domain::model


namespace industrial_gateway::application::ports
{

// ==================================================
// 同步协议连接器接口，由 ConnectionWorker 提供采集线程
// ==================================================
//
// 职责：
// 1. 管理一个连接实例的协议通信资源。
// 2. 建立和断开协议连接。
// 3. 根据逻辑点位配置执行同步读取和数据解码。
// 4. 返回设备任务级的完整采集结果。
//
// 不负责：
// 1. 创建业务采集线程或安排采集周期。
// 2. 构建 TelemetryPacket 或执行 JSON 序列化。
// 3. 操作 DataQueue、SQLite 或 MQTT。
//
// 运行约定：
// 1. 一个实例由一个 ConnectionWorker 管理。
// 2. connect()、disconnect()、readPoints() 由所属 Worker 串行调用。
// 3. 连接和读取应使用协议配置中的有限超时，不在内部无限重试。
// 4. 重试时机和采集调度由 ConnectionWorker 决定。
// 5. isConnected() 可由其他线程查询，具体实现必须保证线程安全。
// 6. 销毁前必须确保所有外部调用已经结束。
//
// ==================================================

class IProtocolConnector
{
public:
    virtual ~IProtocolConnector() = default;


    // ==================================================
    // 连接生命周期
    // ==================================================

    // 同步尝试建立协议连接。
    //
    // true 表示本次操作结束时连接已建立；已连接时重复调用应成功。
    // false 表示连接失败，error_message 描述原因。
    // 成功时清空 error_message。
    // 失败时释放本次建立过程中产生的无效资源，允许随后再次调用。
    // 不启动业务采集线程，也不在内部持续重连。
    [[nodiscard]]
    virtual bool connect(std::string& error_message) = 0;

    // 断开连接并释放协议通信资源，支持重复调用。
    // 由 Worker 在当前同步读取结束后调用，不用于跨线程取消读取。
    // 具体实现的析构函数也应保证释放连接资源。
    virtual void disconnect() noexcept = 0;

    // 返回本地当前已知的连接状态快照，不执行网络探测。
    // true 不保证下一次读取成功，也不代表设备中所有点位均可读取。
    [[nodiscard]]
    virtual bool isConnected() const noexcept = 0;


    // ==================================================
    // 同步采集
    // ==================================================

    // 执行当前连接的一个频率任务。
    //
    // task：
    // 一个频率下的设备及逻辑点位集合。不同频率分别调用本接口。
    // 协议地址、读取类型和解码方式由具体驱动的配置映射决定，
    // 公共任务不携带 Modbus 寄存器或 OPC UA NodeId 等协议字段。
    //
    // results：
    // 每次调用先清空，按任务中的设备顺序返回设备任务级结果。
    // 一个任务可包含多个设备，不代表整个任务只有一次协议请求。
    // 第一版每个设备在该频率下作为完整读取单元：
    // 全部读取并解码成功才返回 Success 和完整记录；
    // 超时、断线、协议异常或解码失败返回 Failure 和空记录。
    // 不返回设备内部点位级的部分成功结果。
    //
    // 返回值：
    // true：任务处理完成，每个设备都有结果；仍可能包含 Failure。
    // false：任务本身无法处理，例如任务不属于本连接、配置映射无效；
    //        清空 results，并通过 error_message 描述原因。
    // 普通设备通信失败通过 results 表达，不应作为任务级 false。
    // 尚未连接时，也应为任务中的设备返回 Failure，而不隐式连接。
    // true 时清空 error_message，设备错误保存在各自结果中。
    //
    // 本接口阻塞调用线程；执行期间不得并发调用其他连接操作。
    // Worker 在本调用完成后记录包的 UTC 完成时间，再组包入队。
    [[nodiscard]]
    virtual bool readPoints(
        const domain::model::CollectionTask& task,
        std::vector<domain::model::CollectionResult>& results,
        std::string& error_message
    ) = 0;


    // ==================================================
    // 实例标识
    // ==================================================

    // 连接实例唯一 ID，构造后不变，返回引用在实例存活期间有效。
    [[nodiscard]]
    virtual const std::string& connectionId() const noexcept = 0;

    // 固定协议标识，例如 modbus_tcp / opcua。
    // 构造后不变，返回引用在实例存活期间有效。
    [[nodiscard]]
    virtual const std::string& protocolType() const noexcept = 0;

    // 协议内的通信端点标识，由驱动规范化，构造后不变，可跨线程读取。
    // Modbus TCP 为数字 IPv4:端口，不包含 connection_id、Unit ID 或采集周期。
    // 管理器使用 (protocolType, endpointKey) 拒绝同一端点的重复采集线程；
    // 后续协议自行定义其物理连接的标识，公共管理层不解析协议配置。
    [[nodiscard]]
    virtual const std::string& endpointKey() const noexcept = 0;
};

} // namespace industrial_gateway::application::ports
