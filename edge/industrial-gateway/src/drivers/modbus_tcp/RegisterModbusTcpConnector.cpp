#include "RegisterModbusTcpConnector.hpp"

#include "ModbusTcpConfigParser.hpp"
#include "ModbusTcpConnector.hpp"
#include "../../application/connector/ConnectorFactoryRegistry.hpp"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace industrial_gateway::drivers::modbus_tcp
{
namespace
{
using application::connector::ConnectorCreationResult;
using domain::model::CollectionTask;
using domain::model::DeviceCollectionTask;
using Interval = std::chrono::milliseconds::rep;

// 配置已经通过协议校验：设备 ID 唯一，设备内部点位名称唯一，
// 同设备同周期的点位满足一次寄存器请求的约束。
// 此处只组织逻辑任务，不重新解释协议地址，也不拆分或合并不同频率。
std::vector<CollectionTask> buildTasks(
    const std::string& connection_id,
    const ModbusTcpConfig& config
)
{
    // 按周期升序输出任务；相同到期时间下，调度器会按此顺序执行。
    std::map<Interval, CollectionTask> tasks_by_interval;

    // 外层按配置顺序遍历设备，确保各频率任务中的设备顺序稳定。
    for (const auto& device : config.devices)
    {
        std::map<Interval, DeviceCollectionTask> device_groups;
        for (const auto& point : device.points)
        {
            auto& group = device_groups[point.interval.count()];
            group.device_id = device.device_id;
            // 只复制逻辑名称，保留该设备在该周期下的点位配置顺序。
            // 地址、Unit ID、解码规则仍由驱动持有。
            group.properties.push_back(point.property);
        }

        for (auto& entry : device_groups)
        {
            auto& task = tasks_by_interval[entry.first];
            task.connection_id = connection_id;
            task.interval = std::chrono::milliseconds{entry.first};
            task.devices.push_back(std::move(entry.second));
        }
    }

    std::vector<CollectionTask> tasks;
    tasks.reserve(tasks_by_interval.size());
    for (auto& entry : tasks_by_interval)
        tasks.push_back(std::move(entry.second));
    return tasks;
}

// 两种来源都在完成解析及语义校验后调用此函数。
// 驱动构造会检查数字 IPv4、可表示的超时等运行约束，但不会连接设备。
// 使用临时结果保证构造失败时不覆盖调用方已有结果。
void buildConnector(const std::string& connection_id, ModbusTcpConfig protocol_config,
                    common::ILogger& logger, ConnectorCreationResult& result)
{
    ConnectorCreationResult candidate;
    candidate.tasks = buildTasks(connection_id, protocol_config);
    candidate.connector = std::make_unique<ModbusTcpConnector>(
        connection_id, std::move(protocol_config), logger);
    result = std::move(candidate);
}

// 静态文件配置入口，驱动本身不感知 YAML 文件。
bool createModbusTcpConnector(
    const config::ConnectionDefinition& definition,
    common::ILogger& logger,
    ConnectorCreationResult& result,
    std::string& error_message
)
{
    error_message.clear();
    const auto fail = [&](const std::string& reason)
    {
        error_message = "Modbus TCP 配置文件 '" + definition.config_file + "': " + reason;
        return false;
    };

    try
    {
        // 路径已由上层解析为绝对路径，注册表已检查公共连接定义。
        // 文件读取在实际 create() 时发生，不在 registerFactory() 时发生。
        const auto root = YAML::LoadFile(definition.config_file);
        ModbusTcpConfig protocol_config;
        std::string parse_error;
        if (!ModbusTcpConfigParser{}.parse(root, protocol_config, parse_error))
            return fail(parse_error);

        buildConnector(definition.connection_id, std::move(protocol_config), logger, result);
        return true;
    }
    catch (const YAML::Exception& error)
    {
        return fail(std::string("读取或解析 YAML 失败: ") + error.what());
    }
    catch (const std::exception& error)
    {
        return fail(std::string("构建驱动或采集任务失败: ") + error.what());
    }
    catch (...)
    {
        return fail("创建 Modbus TCP 连接实例时发生未知异常");
    }
}

// 动态内存配置入口，直接解析对象文本，不生成临时文件或转换为 YAML。
bool createModbusTcpConnectorFromMemory(
    const config::CollectionConnectionConfig& definition,
    common::ILogger& logger,
    ConnectorCreationResult& result,
    std::string& error_message
)
{
    error_message.clear();
    const auto fail = [&](const std::string& reason)
    {
        error_message = "Modbus TCP 内存配置: " + reason;
        return false;
    };

    try
    {
        ModbusTcpConfig protocol_config;
        std::string parse_error;
        if (!ModbusTcpConfigParser{}.parseJson(definition.config_json, protocol_config, parse_error))
            return fail(parse_error);

        buildConnector(definition.connection_id, std::move(protocol_config), logger, result);
        return true;
    }
    catch (const std::exception& error)
    {
        return fail(std::string("构建驱动或采集任务失败: ") + error.what());
    }
    catch (...)
    {
        return fail("创建 Modbus TCP 连接实例时发生未知异常");
    }
}

} // namespace


bool registerModbusTcpConnector(
    application::connector::ConnectorFactoryRegistry& registry,
    std::string& error_message
)
{
    return registry.registerFactory("modbus_tcp", createModbusTcpConnector,
                                    createModbusTcpConnectorFromMemory, error_message);
}

} // namespace industrial_gateway::drivers::modbus_tcp
