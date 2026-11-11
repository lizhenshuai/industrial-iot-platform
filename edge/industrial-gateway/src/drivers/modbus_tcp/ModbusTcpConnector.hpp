#pragma once

#include "ModbusTcpConfig.hpp"
#include "industrial_gateway/application/ports/IProtocolConnector.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/domain/model/CollectionResult.hpp"
#include "industrial_gateway/domain/model/CollectionTask.hpp"

#include <memory>
#include <string>
#include <vector>

namespace industrial_gateway::drivers::modbus_tcp
{

// 一个 Modbus TCP 端点的同步协议驱动，不创建业务线程，不操作数据队列。
// ConnectionWorker 串行调用 connect()/readPoints()/disconnect()；
// isConnected() 可跨线程查询，返回本地状态快照。
class ModbusTcpConnector final : public application::ports::IProtocolConnector
{
public:
    // 保存配置副本并校验，不建立网络连接。
    // connection_id 非空；logger 必须比本对象存活更久。
    // 第一版 host 必须为数字 IPv4 地址，避免域名解析突破连接超时。
    // 非法配置或无法表示的超时抛出 std::invalid_argument。
    ModbusTcpConnector(
        std::string connection_id,
        ModbusTcpConfig config,
        common::ILogger& logger
    );
    ~ModbusTcpConnector() override;

    ModbusTcpConnector(const ModbusTcpConnector&) = delete;
    ModbusTcpConnector& operator=(const ModbusTcpConnector&) = delete;
    ModbusTcpConnector(ModbusTcpConnector&&) = delete;
    ModbusTcpConnector& operator=(ModbusTcpConnector&&) = delete;

    // 单次同步连接，使用 connect_timeout；已连接时直接成功。
    // 不自行重连；失败后允许上层再次调用。
    [[nodiscard]]
    bool connect(std::string& error_message) override;

    // 幂等释放连接和 libmodbus 上下文；不得与 connect/readPoints 并发调用。
    void disconnect() noexcept override;

    [[nodiscard]]
    bool isConnected() const noexcept override;

    // task 必须完整覆盖本配置中该周期的设备及点位，不允许跨周期混读。
    // 先校验全部映射，再按任务中的设备顺序同步读取。
    // 每设备只发送一次连续寄存器读取请求，按 properties 顺序返回记录。
    // 网络或设备故障通过 CollectionResult::Failure 表达，返回 true；
    // 非法任务返回 false，清空 results 并填写 error_message。
    // 超时或报文异常后断开连接，避免迟到响应污染下一次读取。
    [[nodiscard]]
    bool readPoints(
        const domain::model::CollectionTask& task,
        std::vector<domain::model::CollectionResult>& results,
        std::string& error_message
    ) override;

    [[nodiscard]]
    const std::string& connectionId() const noexcept override;

    [[nodiscard]]
    const std::string& protocolType() const noexcept override;

    [[nodiscard]]
    const std::string& endpointKey() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace industrial_gateway::drivers::modbus_tcp
