
#pragma once

#include "industrial_gateway/config/LoggerConfig.hpp"

#include <string>


// 前向声明，避免在头文件中引入 yaml-cpp
namespace YAML
{
    class Node;
}


namespace industrial_gateway::infrastructure::config
{

// ==================================================
// 日志配置解析器
// ==================================================

class LoggerConfigParser final
{
public:

    // 解析日志配置
    //
    // root:          YAML 配置文件根节点
    // config:        输出解析后的日志配置
    // error_message: 解析失败时的错误信息
    //
    // 返回值：
    // true  - 解析成功
    // false - 解析失败
    bool parse(
        const YAML::Node& root,
        ::industrial_gateway::config::LoggerConfig& config,
        std::string& error_message
    ) const;

};

} // namespace industrial_gateway::infrastructure::config
