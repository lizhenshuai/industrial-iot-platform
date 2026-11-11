#pragma once

#include "ModbusRtuConfig.hpp"
#include "industrial_gateway/application/ports/IProtocolConnector.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/domain/model/CollectionResult.hpp"
#include "industrial_gateway/domain/model/CollectionTask.hpp"

#include <memory>
#include <string>
#include <vector>

namespace industrial_gateway::drivers::modbus_rtu
{
// 一个本地串口对应一个同步驱动，由 ConnectionWorker 提供唯一采集线程。
// 不创建线程、不操作业务队列/SQLite/MQTT，不在内部无限重连或重试读取。
class ModbusRtuConnector final : public application::ports::IProtocolConnector
{
public:
    // 构造仅校验配置及生成端点标识，不打开串口；设备尚未出现不属于坏配置。
    ModbusRtuConnector(std::string connection_id, ModbusRtuConfig config, common::ILogger& logger);
    ~ModbusRtuConnector() override;
    ModbusRtuConnector(const ModbusRtuConnector&) = delete;
    ModbusRtuConnector& operator=(const ModbusRtuConnector&) = delete;
    ModbusRtuConnector(ModbusRtuConnector&&) = delete;
    ModbusRtuConnector& operator=(ModbusRtuConnector&&) = delete;

    // 打开、配置并独占串口。成功只表示串口资源可用，不保证从站在线。
    [[nodiscard]] bool connect(std::string& error_message) override;
    void disconnect() noexcept override;
    [[nodiscard]] bool isConnected() const noexcept override;

    // 先校验完整任务再按原顺序读取；一设备一频率一次连续寄存器请求。
    // 通信失败返回设备 Failure，非法任务返回 false。结果保持任务原顺序。
    // 普通超时/CRC 错误经有限恢复等待后继续；串口 I/O 故障释放资源等 Worker 重开。
    [[nodiscard]] bool readPoints(const domain::model::CollectionTask& task,
        std::vector<domain::model::CollectionResult>& results, std::string& error_message) override;

    [[nodiscard]] const std::string& connectionId() const noexcept override;
    [[nodiscard]] const std::string& protocolType() const noexcept override;
    // 已存在字符设备用设备号标识，未出现的路径用规范路径标识；不含从站或波特率。
    [[nodiscard]] const std::string& endpointKey() const noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace industrial_gateway::drivers::modbus_rtu
