#pragma once

#include "ModbusTcpConfig.hpp"

#include <string>


// 避免在头文件中引入 yaml-cpp 的具体实现。
namespace YAML
{
    class Node;
}


namespace industrial_gateway::drivers::modbus_tcp
{

// ==================================================
// Modbus TCP 协议配置解析器
// ==================================================
//
// 职责：
// 1. 将协议专属 YAML 或 JSON 配置转换为 ModbusTcpConfig。
// 2. 校验连接、设备、点位字段以及跨点位的组合约束。
// 3. 在启动采集前拒绝不满足“一设备一频率一次请求”的配置。
//
// 不负责读取文件、建立连接、生成采集线程或执行设备探测。
// 文件路径由公共配置加载流程解析，调用方读取 YAML 后传入根节点。
// JSON 入口接受 CollectionConnectionConfig::config_json 的对象文本。
// connection_id、protocol 由公共连接定义提供，config_file 仅用于静态入口，
// 不在协议专属 YAML / JSON 中重复定义。
//
// ==================================================

class ModbusTcpConfigParser final
{
public:
    // ==================================================
    // 解析并校验协议配置
    // ==================================================

    // root 是协议配置文件的根节点，包含 connection 和 devices：
    //
    // connection:
    //   host: "192.168.1.100"
    //   port: 502
    //   connect_timeout_ms: 1000
    //   response_timeout_ms: 1000
    // devices:
    //   - device_id: 10001
    //     unit_id: 1
    //     points:
    //       - property: "temperature"
    //         register_type: "holding"
    //         address: 0
    //         data_type: "float32"
    //         byte_order: "high_byte_first"
    //         word_order: "high_word_first"
    //         interval_ms: 1000
    //         scale: 1.0
    //         offset: 0.0
    //
    // 必填：connection、connection.host、devices；
    // 每个设备的 device_id、points；每个点位的 property、address。
    // 其他字段缺省时使用 ModbusTcpConfig 及其成员结构的默认值。
    // 显式提供的字段必须有效，不能将错误值当作缺省值处理。
    //
    // 枚举字符串区分大小写：
    // register_type：holding / input。
    // data_type：int16 / uint16 / int32 / uint32 /
    //            int64 / uint64 / float32 / float64。
    // byte_order：high_byte_first / low_byte_first。
    // word_order：high_word_first / low_word_first。
    //
    // 拒绝未知字段、重复映射键、错误节点类型和无法转换的字段值。
    // 整数先以足够宽的类型读取并检查范围，再转换为目标类型，避免截断。
    // 使用临时配置完成解析及 validate()，全部成功后才更新 config。
    //
    // true：解析、校验成功，更新 config 并清空 error_message。
    // false：失败，config 保持不变，error_message 包含字段路径或设备、点位信息。
    // YAML 格式及字段转换异常应转换为 false 和错误信息。
    [[nodiscard]]
    bool parse(
        const YAML::Node& root,
        ModbusTcpConfig& config,
        std::string& error_message
    ) const;


    // ==================================================
    // 解析并校验内存中的协议 JSON 配置
    // ==================================================

    // config_json 是单个连接的 config 对象文本，不是 MQTT 完整快照。
    // 字段层次、必填项、枚举名称、默认值及语义约束与 YAML 入口一致。
    //
    // 严格使用 JSON 类型：
    // - host、property 及枚举值必须是字符串。
    // - device_id、unit_id、address、port、周期和超时必须是 JSON 整数，
    //   不接受字符串、布尔值、小数或指数写法，并在窄化前检查范围。
    // - scale、offset 接受整数或浮点数，转换后的 double 必须有限。
    // - 可选字段只有缺失时采用默认值，显式 null 不视为缺省。
    //
    // 独立检查 JSON 语法、所有层级的重复键、未知字段和节点类型，
    // 不要求调用方必须先经过公共快照解析器；不接受注释或尾随内容。
    // 与 YAML 入口共用点位字段读取逻辑和 validate()，避免采集规则分叉。
    //
    // true：完整解析及校验通过，更新 config，清空 error_message。
    // false：保留原 config，错误包含字段路径、重复键或语法错误位置。
    // 不读取文件、不构建任务、不创建驱动，也不探测设备可达性。
    [[nodiscard]]
    bool parseJson(
        const std::string& config_json,
        ModbusTcpConfig& config,
        std::string& error_message
    ) const;


    // ==================================================
    // 校验已构造的配置
    // ==================================================

    // 供 parse()、parseJson() 复用，也供直接通过 C++ 构造配置的调用方使用。
    // 不修改 config，不访问设备。
    //
    // 校验范围：
    // 1. 主机地址非空、端口有效、连接及响应超时大于 0。
    // 2. 设备集合非空，device_id 在本连接内不重复，Unit ID 有效。
    // 3. 点位集合非空，property 非空且在本设备内不重复。
    // 4. 所有枚举值有效、周期大于 0、scale 和 offset 为有限数值。
    // 5. 根据 data_type 推导寄存器数量，检查完整点位地址范围不越界。
    // 6. 按设备和周期分组，确保同组 register_type 一致。
    // 7. 同组全部点位的连续覆盖范围不能超过单次读取允许的数量。
    //    不满足时拒绝配置，不自动拆分任务或请求。
    //
    // 地址间隙内寄存器是否真实可读，无法仅凭配置确定，需设备联调验证。
    // 配置校验成功也不保证运行时设备在线或一定返回有效数据。
    //
    // true：校验成功，清空 error_message。
    // false：校验失败，通过 error_message 描述具体原因。
    [[nodiscard]]
    bool validate(
        const ModbusTcpConfig& config,
        std::string& error_message
    ) const;
};

} // namespace industrial_gateway::drivers::modbus_tcp
