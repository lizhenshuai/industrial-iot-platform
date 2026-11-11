#pragma once

#include "OpcUaConfig.hpp"
#include "industrial_gateway/application/ports/IProtocolConnector.hpp"
#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/domain/model/CollectionResult.hpp"
#include "industrial_gateway/domain/model/CollectionTask.hpp"
#include <memory>

namespace industrial_gateway::drivers::opcua
{
// 一个 Endpoint 对应一个同步客户端，所有协议操作由所属 Worker 串行执行。
// 不创建业务线程、不操作队列/SQLite/MQTT。isConnected 返回原子状态快照。
class OpcUaConnector final : public application::ports::IProtocolConnector
{
public:
    OpcUaConnector(std::string connection_id, OpcUaConfig config, common::ILogger& logger);
    ~OpcUaConnector() override;
    OpcUaConnector(const OpcUaConnector&) = delete;
    OpcUaConnector& operator=(const OpcUaConnector&) = delete;
    OpcUaConnector(OpcUaConnector&&) = delete;
    OpcUaConnector& operator=(OpcUaConnector&&) = delete;

    [[nodiscard]] bool connect(std::string& error) override;
    void disconnect() noexcept override;
    [[nodiscard]] bool isConnected() const noexcept override;
    // 全任务先校验；每设备每周期一次 Read，不自动分批、不返回部分点位成功。
    [[nodiscard]] bool readPoints(const domain::model::CollectionTask& task,
        std::vector<domain::model::CollectionResult>& results, std::string& error) override;
    [[nodiscard]] const std::string& connectionId() const noexcept override;
    [[nodiscard]] const std::string& protocolType() const noexcept override;
    [[nodiscard]] const std::string& endpointKey() const noexcept override;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace industrial_gateway::drivers::opcua
