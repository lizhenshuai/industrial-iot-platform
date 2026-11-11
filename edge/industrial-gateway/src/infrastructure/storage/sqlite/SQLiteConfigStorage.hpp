#pragma once

#include "industrial_gateway/application/ports/IConfigStorage.hpp"
#include "industrial_gateway/config/StorageConfig.hpp"
#include "SQLiteDatabase.hpp"

namespace industrial_gateway::infrastructure::storage::sqlite
{
// 独立连接可与遥测存储使用相同 database_path，但不能共享连接实例。
class SQLiteConfigStorage final : public application::ports::IConfigStorage
{
public:
    explicit SQLiteConfigStorage(const ::industrial_gateway::config::StorageConfig& config);
    bool initialize(std::string& error_message) override;
    bool load(std::optional<application::ports::StoredConfig>& config,
              std::string& error_message) override;
    bool save(const application::ports::StoredConfig& config,
              std::string& error_message) override;
private:
    const ::industrial_gateway::config::StorageConfig config_;
    SQLiteDatabase database_;
    bool initialized_{false};
};
} // namespace industrial_gateway::infrastructure::storage::sqlite
