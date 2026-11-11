#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace industrial_gateway::application::ports
{
struct StoredConfig final
{
    std::int64_t updated_at_ms{0};
    std::string config_json;
};

// 只保存网关唯一的目标配置，不保存历史版本或应用状态。
// 一个实例由配置管理线程独占，生命周期覆盖 ConfigManager。
class IConfigStorage
{
public:
    virtual ~IConfigStorage() = default;
    virtual bool initialize(std::string& error_message) = 0;
    // true + nullopt 表示没有记录；false 表示读取失败，保持输出不变。
    virtual bool load(std::optional<StoredConfig>& config, std::string& error_message) = 0;
    // 调用方先完整校验 JSON；一次原子覆盖正文和时间，失败保留原记录。
    virtual bool save(const StoredConfig& config, std::string& error_message) = 0;
};
} // namespace industrial_gateway::application::ports
