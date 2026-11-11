#include "StorageWorker.hpp"

#include "industrial_gateway/domain/enums/CollectionStatus.hpp"
#include "industrial_gateway/domain/types/TelemetryValue.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>


namespace industrial_gateway::application::ingestion
{

namespace
{

// ==================================================
// 日志组件名称
// ==================================================

constexpr const char* kLogComponent = "StorageWorker";


// 保留现有字段的插入顺序；字符串转义和数值格式化统一交给 JSON 库。
using Json = nlohmann::ordered_json;


// ==================================================
// 采集状态转换为字符串
// ==================================================

const char* collectionStatusToString(
    domain::enums::CollectionStatus status
) noexcept
{
    switch (status)
    {
        case domain::enums::CollectionStatus::Success:
            return "Success";

        case domain::enums::CollectionStatus::Failure:
            return "Failure";

        default:
            return "Unknown";
    }
}


// ==================================================
// 将采集值转换为 JSON 值，保留 variant 的实际类型
// ==================================================

bool telemetryValueToJson(
    const domain::types::TelemetryValue& value,
    Json& json,
    std::string& error_message
)
{
    error_message.clear();
    if (value.valueless_by_exception())
    {
        error_message = "不支持的 TelemetryValue 类型";
        return false;
    }
    // JSON 库会把非有限浮点数序列化成 null；这里先检查，以保留原有诊断。
    if (const auto* number = std::get_if<double>(&value); number && !std::isfinite(*number))
    {
        error_message = "TelemetryValue 中的 double 不是有限数值";
        return false;
    }

    // bool、int64_t、uint64_t、double、string 分别赋值，不经字符串或 double 中转。
    // 特别是 uint64_t 的大整数，不能为了统一数值类型而转换成 double。
    std::visit([&json](const auto& item) { json = item; }, value);
    return true;
}


} // namespace


// ==================================================
// 构造函数
// ==================================================

StorageWorker::StorageWorker(
    ports::IDataQueue& data_queue,
    ports::IMessageStorage& message_storage,
    common::ILogger& logger
)
    : data_queue_(data_queue),
      message_storage_(message_storage),
      logger_(logger)
{
}


// ==================================================
// 完整包序列化
// ==================================================
// 设备/点位异常保留原诊断，不拆包；合法 UTF-8 的包 ID 原样保存。
bool StorageWorker::serializePacket(
    const domain::model::TelemetryPacket& packet,
    std::string& json,
    std::string& error_message
)
{
    json.clear();
    error_message.clear();
    const auto report = [&](const std::string& error)
    {
        if (!error_message.empty()) error_message += "；";
        error_message += error;
    };

    Json packet_json = Json::object();
    packet_json["message_id"] = packet.message_id;
    packet_json["timestamp_ms"] = packet.timestamp_ms;
    packet_json["devices"] = Json::array();
    for (const auto& device : packet.devices)
    {
        Json device_json = Json::object();
        device_json["device_id"] = device.device_id;
        device_json["status"] = collectionStatusToString(device.status);
        device_json["records"] = Json::array();
        for (const auto& record : device.records)
        {
            Json record_json = Json::object();
            record_json["property"] = record.property;
            Json value_json;
            std::string value_error;
            if (telemetryValueToJson(record.value, value_json, value_error))
            {
                record_json["value"] = std::move(value_json);
            }
            else
            {
                record_json["value"] = nullptr;
                record_json["serialization_error"] = value_error;
                record_json["value_serialization_error"] = value_error;
                if (const auto* number = std::get_if<double>(&record.value);
                    number && !std::isfinite(*number))
                {
                    record_json["value_raw"] = std::isnan(*number) ? "NaN" :
                        (std::signbit(*number) ? "-Infinity" : "Infinity");
                }
                report("device_id=" + std::to_string(device.device_id) +
                       "，property=" + record.property + ": " + value_error);
            }
            device_json["records"].push_back(std::move(record_json));
        }

        std::string device_error;
        if (device.status != domain::enums::CollectionStatus::Success &&
            device.status != domain::enums::CollectionStatus::Failure)
        {
            device_error = "未知设备采集状态";
            device_json["status_code"] = static_cast<int>(device.status);
        }
        else if (device.status == domain::enums::CollectionStatus::Failure &&
                 !device.records.empty())
        {
            device_error = "Failure 设备的 records 应为空";
        }
        if (!device_error.empty())
        {
            // 保留不符合约定的输入并标记诊断，避免静默丢失点位数据。
            device_json["serialization_error"] = device_error;
            report("device_id=" + std::to_string(device.device_id) + ": " + device_error);
        }
        packet_json["devices"].push_back(std::move(device_json));
    }

    try
    {
        // 默认严格检查 UTF-8；正常字符串（包括中文）保持原文，不强制 ASCII 转义。
        json = packet_json.dump();
    }
    catch (const Json::type_error& error)
    {
        if (error.id != 316) throw;
        // 只有非法 UTF-8 使用替换策略，避免一个坏字符串丢掉整包其他设备数据。
        // 替换会改变损坏的字符串（包括可能损坏的 message_id），因此必须显式诊断。
        // 内存分配等执行异常继续向上传递，不伪装成已生成有效诊断消息。
        const std::string encoding_error = "字符串包含非法 UTF-8，已用 U+FFFD 替换无效字节序列";
        packet_json["serialization_error"] = encoding_error;
        report(encoding_error);
        json = packet_json.dump(-1, ' ', false, Json::error_handler_t::replace);
    }
    return error_message.empty();
}


// ==================================================
// 批量数据转换
// ==================================================

bool StorageWorker::serializeBatch(
    const std::vector<domain::model::TelemetryPacket>& records,
    std::vector<ports::MessageToStore>& messages,
    std::string& error_message
)
{
    messages.clear();

    error_message.clear();


    // ==================================================
    // 1. 空批次处理
    // ==================================================

    if (records.empty())
    {
        return true;
    }


    // ==================================================
    // 2. 预留空间
    // ==================================================

    messages.reserve(records.size());


    // ==================================================
    // 3. 逐条序列化
    // ==================================================

    for (
        std::size_t i = 0;
        i < records.size();
        ++i
    )
    {
        std::string json;

        std::string record_error;


        if (
            !serializePacket(
                records[i],
                json,
                record_error
            )
        )
        {
            // serializePacket 已在完整包中保留异常诊断。
            // 不因单个异常点位丢弃同包中的其他设备和记录。

            if (!error_message.empty())
            {
                error_message += "；";
            }

            error_message +=
                "第 " +
                std::to_string(i) +
                " 个包（message_id=" +
                records[i].message_id +
                "）已转换为诊断消息，原因: " +
                record_error;
        }


        // --------------------------------------------------
        // 构造待存储消息
        // --------------------------------------------------

        ports::MessageToStore message;

        message.timestamp_ms =
            records[i].timestamp_ms;

        message.message =
            std::move(json);


        messages.push_back(
            std::move(message)
        );
    }


    // 注意：
    // 数据级序列化异常已经转换为诊断消息。
    // 只要诊断消息构造完成，整个批次就可以继续持久化。
    return true;
}


// ==================================================
// StorageWorker 工作主循环
// ==================================================

void StorageWorker::run()
{
    logger_.info(
        kLogComponent,
        "StorageWorker 开始运行"
    );


    // ==================================================
    // 1. 初始化 SQLite 消息存储
    // ==================================================

    std::string error_message;

    bool initialization_failed = false;

    while (true)
    {
        error_message.clear();

        if (
            message_storage_.initialize(
                error_message
            )
        )
        {
            break;
        }


        // 首次失败时记录日志
        if (!initialization_failed)
        {
            logger_.error(
                kLogComponent,
                "SQLite 消息存储初始化失败: " +
                error_message
            );

            initialization_failed = true;
        }


        // 如果队列已经关闭且没有剩余数据，
        // 则不需要继续等待数据库恢复。
        if (
            data_queue_.isClosed() &&
            data_queue_.size() == 0
        )
        {
            logger_.warn(
                kLogComponent,
                "DataQueue 已关闭且没有待处理数据，"
                "StorageWorker 退出"
            );

            return;
        }


        // 等待后重新初始化
        std::this_thread::sleep_for(
            kRetryInterval
        );
    }


    if (initialization_failed)
    {
        logger_.info(
            kLogComponent,
            "SQLite 消息存储已恢复，初始化成功"
        );
    }
    else
    {
        logger_.info(
            kLogComponent,
            "SQLite 消息存储初始化成功"
        );
    }


    // ==================================================
    // 2. 准备批量处理容器
    // ==================================================

    std::vector<domain::model::TelemetryPacket> record_batch;

    std::vector<ports::MessageToStore> message_batch;


    // ==================================================
    // 3. 消费 DataQueue
    // ==================================================

    while (true)
    {
        const auto pop_status =
            data_queue_.waitPopBatch(
                record_batch,
                kBatchSize,
                kBatchWaitTimeout
            );


        // --------------------------------------------------
        // 队列等待超时
        // --------------------------------------------------

        if (
            pop_status ==
            ports::DataQueuePopStatus::Timeout
        )
        {
            continue;
        }


        // --------------------------------------------------
        // 队列已经关闭且数据排空
        // --------------------------------------------------

        if (
            pop_status ==
            ports::DataQueuePopStatus::Closed
        )
        {
            break;
        }


        // --------------------------------------------------
        // 获取到数据
        // --------------------------------------------------

        if (
            pop_status !=
            ports::DataQueuePopStatus::DataAvailable
        )
        {
            logger_.error(
                kLogComponent,
                "DataQueue 返回未知消费状态"
            );

            continue;
        }


        if (record_batch.empty())
        {
            logger_.warn(
                kLogComponent,
                "DataQueue 返回 DataAvailable，"
                "但批次为空"
            );

            continue;
        }


        // ==================================================
        // 4. 序列化当前批次
        // ==================================================

        error_message.clear();

        if (
            !serializeBatch(
                record_batch,
                message_batch,
                error_message
            )
        )
        {
            // serializeBatch 已经负责将单条数据异常转换为
            // 诊断消息。这里的失败意味着批次转换本身未完成。
            //
            // 不对确定性的序列化失败进行无限重试。

            logger_.critical(
                kLogComponent,
                "采集数据批次转换失败，无法继续持久化: " +
                error_message
            );

            return;
        }


        // 有异常记录时，记录一次严重日志。
        // 异常记录已经转换为诊断 JSON，仍会随批次写入 SQLite。
        if (!error_message.empty())
        {
            logger_.critical(
                kLogComponent,
                "发现无法按标准格式序列化的采集记录，"
                "已转换为诊断消息并继续持久化: " +
                error_message
            );
        }


        // ==================================================
        // 5. 批量写入 SQLite
        // ==================================================

        bool storage_error_logged = false;

        bool recovery_error_logged = false;

        // 初始数据库连接已经初始化成功。
        bool storage_ready = true;

        while (true)
        {
            // --------------------------------------------------
            // 数据库恢复 / 重新初始化
            // --------------------------------------------------

            if (!storage_ready)
            {
                std::string recovery_error;

                if (
                    !message_storage_.initialize(
                        recovery_error
                    )
                )
                {
                    if (!recovery_error_logged)
                    {
                        logger_.error(
                            kLogComponent,
                            "SQLite 消息存储重新初始化失败: " +
                            recovery_error
                        );

                        recovery_error_logged = true;
                    }

                    std::this_thread::sleep_for(
                        kRetryInterval
                    );

                    continue;
                }

                storage_ready = true;

                if (recovery_error_logged)
                {
                    logger_.info(
                        kLogComponent,
                        "SQLite 消息存储重新初始化成功"
                    );

                    recovery_error_logged = false;
                }
            }


            // --------------------------------------------------
            // 尝试写入当前批次
            // --------------------------------------------------

            error_message.clear();

            if (
                message_storage_.insertBatch(
                    message_batch,
                    error_message
                )
            )
            {
                break;
            }


            // 记录首次数据库写入错误
            if (!storage_error_logged)
            {
                logger_.error(
                    kLogComponent,
                    "SQLite 批量写入失败，保留当前批次并重试: " +
                    error_message
                );

                storage_error_logged = true;
            }


            // 写入失败后，下一轮必须先调用 initialize()。
            //
            // 如果 SQLiteMessageStorage 因回滚失败而将
            // initialized_ 置为 false，initialize() 会重新建立连接。
            //
            // 如果只是暂时的 SQLITE_BUSY 等错误，
            // initialize() 会保持现有连接，随后重新尝试写入。
            storage_ready = false;

            std::this_thread::sleep_for(
                kRetryInterval
            );
        }


        // ==================================================
        // 6. 写入成功
        // ==================================================

        if (storage_error_logged)
        {
            logger_.info(
                kLogComponent,
                "SQLite 批量写入已恢复"
            );
        }


        logger_.debug(
            kLogComponent,
            "成功持久化 " +
            std::to_string(message_batch.size()) +
            " 个采集包"
        );


        // ==================================================
        // 7. 清理当前批次
        // ==================================================

        record_batch.clear();

        message_batch.clear();
    }


    // ==================================================
    // 8. 工作线程退出
    // ==================================================

    logger_.info(
        kLogComponent,
        "DataQueue 已关闭且剩余数据排空，"
        "StorageWorker 正常退出"
    );
}

} // namespace industrial_gateway::application::ingestion
