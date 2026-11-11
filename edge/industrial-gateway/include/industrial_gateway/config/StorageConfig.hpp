#pragma once

#include <string>


namespace industrial_gateway::config
{

// ==================================================
// SQLite 存储配置
// ==================================================

struct StorageConfig
{
    // SQLite 数据库文件路径
    std::string database_path = "data/gateway.db";

    // 数据库忙等待超时时间（毫秒）
    int busy_timeout_ms = 5000;
};

} // namespace industrial_gateway::config