#include "SQLiteConfigStorage.hpp"

#include <filesystem>
#include <utility>

namespace industrial_gateway::infrastructure::storage::sqlite
{
SQLiteConfigStorage::SQLiteConfigStorage(const ::industrial_gateway::config::StorageConfig& config)
    : config_(config) {}

bool SQLiteConfigStorage::initialize(std::string& error_message)
{
    error_message.clear();
    if (initialized_ && database_.isOpen()) return true;
    initialized_ = false;
    if (config_.database_path.empty() || config_.database_path.find('\0') != std::string::npos ||
        config_.busy_timeout_ms < 0)
    {
        error_message = "配置数据库路径或 busy_timeout_ms 非法";
        return false;
    }
    const auto parent = std::filesystem::path(config_.database_path).parent_path();
    std::error_code ec;
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec)
    {
        error_message = "创建配置数据库目录失败: " + ec.message();
        return false;
    }
    if (!database_.isOpen() && !database_.open(config_.database_path, config_.busy_timeout_ms, error_message))
        return false;
    // CHECK 固定主键，保证整张表最多一条记录；不依赖 SQLite JSON 扩展。
    initialized_ = database_.executeRaw(R"(
        CREATE TABLE IF NOT EXISTS gateway_collection_config (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            updated_at_ms INTEGER NOT NULL,
            config_json TEXT NOT NULL
        );
    )", error_message);
    return initialized_;
}

bool SQLiteConfigStorage::load(std::optional<application::ports::StoredConfig>& config,
                               std::string& error_message)
{
    error_message.clear();
    if (!initialized_ || !database_.isOpen())
    {
        error_message = "配置存储尚未初始化";
        return false;
    }
    SQLiteRows rows;
    if (!database_.query("SELECT updated_at_ms, config_json FROM gateway_collection_config WHERE id = 1;",
                         {}, rows, error_message)) return false;
    if (rows.empty())
    {
        config.reset();
        return true;
    }
    if (rows.size() != 1 || rows[0].size() != 2)
    {
        error_message = "配置记录结构异常";
        return false;
    }
    const auto* time = std::get_if<std::int64_t>(&rows[0][0]);
    const auto* json = std::get_if<std::string>(&rows[0][1]);
    if (!time || !json)
    {
        error_message = "配置记录字段类型异常";
        return false;
    }
    config = application::ports::StoredConfig{*time, *json};
    return true;
}

bool SQLiteConfigStorage::save(const application::ports::StoredConfig& config,
                               std::string& error_message)
{
    error_message.clear();
    if (!initialized_ || !database_.isOpen())
    {
        error_message = "配置存储尚未初始化";
        return false;
    }
    if (config.config_json.empty())
    {
        error_message = "配置 JSON 不能为空";
        return false;
    }
    // 单条语句在 SQLite 隐式事务中同时覆盖两个字段，避免先删后插丢失旧配置。
    SQLiteExecuteResult result;
    return database_.execute(R"(
        INSERT INTO gateway_collection_config(id, updated_at_ms, config_json) VALUES(1, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            updated_at_ms = excluded.updated_at_ms, config_json = excluded.config_json;
    )", {config.updated_at_ms, config.config_json}, result, error_message);
}
} // namespace industrial_gateway::infrastructure::storage::sqlite
