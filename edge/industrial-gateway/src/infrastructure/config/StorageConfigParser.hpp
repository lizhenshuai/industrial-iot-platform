
#pragma once

#include "industrial_gateway/config/StorageConfig.hpp"

#include <string>


// 前向声明，避免在头文件中引入 yaml-cpp
namespace YAML
{
    class Node;
}


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// SQLite 存储配置解析器
// ==================================================

class StorageConfigParser final
{
public:

    // 解析 SQLite 存储配置
    //
    // root:          YAML 配置文件根节点
    // config:        输出解析后的存储配置
    // error_message: 解析失败时的错误信息
    //
    // 返回值：
    // true  - 解析成功
    // false - 解析失败
    bool parse(
        const YAML::Node& root,
        ::industrial_gateway::config::StorageConfig& config,
        std::string& error_message
    ) const;

};

} // namespace industrial_gateway::infrastructure::config
