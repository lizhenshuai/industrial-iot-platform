
#pragma once

#include "industrial_gateway/application/ports/IDataQueue.hpp"
#include "industrial_gateway/application/ports/IMessageStorage.hpp"

#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/domain/model/TelemetryPacket.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>


namespace industrial_gateway::application::ingestion
{

// ==================================================
// 采集数据存储工作器
// ==================================================
//
// 核心职责：
//
// 1. 从 DataQueue 批量获取采集数据。
// 2. 将 TelemetryPacket 序列化为 JSON。
// 3. 转换为 MessageToStore。
// 4. 批量写入 SQLite。
// 5. 写入失败时保留当前批次并重试。
//
// 不负责：
//
// 1. 创建和管理 DataQueue。
// 2. 管理 SQLite 底层连接。
// 3. MQTT 消息发布。
// 4. 删除已经发送的消息。
//
// 并发模型：
//
// 1. DataQueue 允许多个生产者。
// 2. StorageWorker 是唯一消费者。
// 3. IMessageStorage 由当前 StorageWorker 独占使用。
// 4. run() 由上层生命周期管理器在线程中执行。
//
// ==================================================

class StorageWorker final
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    // 注入工作所需的依赖。
    //
    // data_queue:
    //     采集数据队列。
    //
    // message_storage:
    //     SQLite 消息存储接口。
    //
    // logger:
    //     日志接口。
    //
    StorageWorker(
        ports::IDataQueue& data_queue,
        ports::IMessageStorage& message_storage,
        common::ILogger& logger
    );


    ~StorageWorker() = default;


    // ==================================================
    // 禁止拷贝和移动
    // ==================================================

    StorageWorker(
        const StorageWorker&
    ) = delete;

    StorageWorker& operator=(
        const StorageWorker&
    ) = delete;

    StorageWorker(
        StorageWorker&&
    ) = delete;

    StorageWorker& operator=(
        StorageWorker&&
    ) = delete;


    // ==================================================
    // 工作主循环
    // ==================================================

    // 执行存储工作循环。
    //
    // 工作流程：
    //
    // 1. 初始化 SQLite 消息存储。
    // 2. 从 DataQueue 获取数据。
    // 3. 将 TelemetryPacket 转换为 JSON。
    // 4. 批量写入 SQLite。
    // 5. 写入失败时重试当前批次。
    // 6. 队列关闭且数据排空后退出。
    //
    // 注意：
    // 由上层生命周期管理器在线程中调用。
    //
    // 调用前应确保依赖对象已经创建，
    // 且其生命周期覆盖整个 run() 执行过程。
    //
    // 退出条件：
    // DataQueue 已关闭，并且剩余数据已经排空。
    //
    void run();


private:

    // ==================================================
    // TelemetryPacket 序列化
    // ==================================================

    // 将一个完整采集包转换为 JSON 字符串。
    //
    // JSON 包含包级 message_id、timestamp_ms 和 devices 数组。
    // 每个设备包含 device_id、status、records；每个点位包含 property、value。
    // 异常字段以嵌套诊断形式保留，仍输出完整包 JSON。
    // 非法 UTF-8 字符串由 JSON 库替换无效序列，并增加包级 serialization_error。
    // 返回 false 表示包含数据诊断，error_message 描述原因。
    // 内存分配等执行异常不在此接口中转换为数据诊断。
    //
    static bool serializePacket(
        const domain::model::TelemetryPacket& packet,
        std::string& json,
        std::string& error_message
    );


    // ==================================================
    // 批量数据转换
    // ==================================================

    // 将一批 TelemetryPacket 转换为 MessageToStore。
    //
    // 每个 TelemetryPacket 对应一条 MessageToStore。
    //
    // timestamp_ms 使用包级时间戳。
    //
    // records:
    //     原始采集包。
    //
    // messages:
    //     转换后的消息集合。
    //
    // error_message:
    //     转换失败时的错误信息。
    //
    // 返回值：
    // true  - 转换成功。
    // false - 转换失败。
    //
    static bool serializeBatch(
        const std::vector<domain::model::TelemetryPacket>& records,
        std::vector<ports::MessageToStore>& messages,
        std::string& error_message
    );


private:

    // ==================================================
    // 外部依赖
    // ==================================================

    // 采集数据队列。
    ports::IDataQueue& data_queue_;

    // 消息存储接口。
    ports::IMessageStorage& message_storage_;

    // 日志接口。
    common::ILogger& logger_;


    // ==================================================
    // 批量处理参数
    // ==================================================

    // 每批最多处理的采集包数量。
    static constexpr std::size_t kBatchSize = 100;

    // 等待批量数据的最长时间。
    inline static constexpr std::chrono::milliseconds
        kBatchWaitTimeout{100};

    // 数据库操作失败后的重试间隔。
    inline static constexpr std::chrono::milliseconds
        kRetryInterval{1000};

};

} // namespace industrial_gateway::application::ingestion
