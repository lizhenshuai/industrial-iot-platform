#pragma once

#include "industrial_gateway/config/CollectionConfigSnapshot.hpp"

#include <optional>
#include <string>


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// MQTT 采集配置快照解析器
// ==================================================
//
// 解析 JSON 消息体的公共结构，不依赖 MQTT 客户端或协议驱动。
// 协议注册状态、协议专属字段、配置应用由后续模块处理。
// JSON 库仅在实现文件中使用，不进入公共配置模型或本头文件的接口。
//
// ==================================================

class CollectionConfigParser final
{
public:
    // payload 必须包含一个完整的 JSON 对象，不接受注释、尾随逗号、
    // 多个根值或未转义的空字符；所有层级的对象均拒绝重复键。
    //
    // 必填公共字段：
    // - schema_version：JSON 整数，当前只支持 1。
    // - request_id：非空、非全空白且不含空字符的字符串。
    // - connections：数组，允许显式 []，不允许缺失或 null。
    //
    // 每个连接必须包含 connection_id、protocol 和 config，
    // 前两者遵守 request_id 的字符串约束，connection_id 在快照内唯一。
    // 根对象和连接对象不允许未知字段；config 必须是 JSON 对象，
    // 内部字段留给协议解析器校验，不在公共层固定协议名称或设备模型。
    //
    // 数字不进行字符串、布尔或浮点到整数的隐式转换。
    // 连接顺序及公共字符串原值保持不变，不自动裁剪或转换大小写。
    // config_json 通过 JSON 对象重新序列化生成，不保留原始空白、
    // 对象成员顺序或数字的原始文本写法，不可用它代替协议语义比较。
    //
    // true：全部解析成功后更新 config，清空 error_message。
    // false：配置输入无效，config 保持不变，error_message 包含
    //        字段路径、重复键信息或 JSON 语法错误的字节位置。
    // 成功仅表示公共配置合法，不表示协议配置可用或已应用。
    // request_id 可选输出用于失败 ACK：先清空，在整份 JSON 语法和重复键
    // 检查通过后，独立提取合法请求 ID；其他公共字段错误不丢失请求关联。
    // 它不表示其余字段已校验成功；缺失或非法请求 ID 返回 nullopt。
    [[nodiscard]]
    bool parse(
        const std::string& payload,
        ::industrial_gateway::config::CollectionConfigSnapshot& config,
        std::string& error_message,
        std::optional<std::string>* request_id = nullptr
    ) const;
};

} // namespace industrial_gateway::infrastructure::config
