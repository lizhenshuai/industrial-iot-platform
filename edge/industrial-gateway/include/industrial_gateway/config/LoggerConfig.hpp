
#pragma once

#include "industrial_gateway/common/LogLevel.hpp"

#include <cstddef>
#include <string>


namespace industrial_gateway::config
{

// ==================================================
// 日志滚动策略
// ==================================================

// 日志滚动策略，只允许按照大小来滚动
enum class LogRotationStrategy
{
    Size
};


// ==================================================
// 日志滚动配置，这个是一个默认配置，默认读取的配置
// ==================================================
// 结构体，存储日志配置信息
struct LogRotationConfig
{
    // 滚动策略，获取滚动策略
    LogRotationStrategy strategy =
        LogRotationStrategy::Size;

    // 单个日志文件最大大小（字节）
    std::size_t max_size_bytes =
        10 * 1024 * 1024;

    // 保留的历史轮转文件数量
    std::size_t max_files = 5;
};


// ==================================================
// 日志配置
// ==================================================

struct LoggerConfig
{
    // 日志等级
    common::LogLevel level =
        common::LogLevel::Info;

    // 是否输出到控制台
    bool console_enabled = true;

    // 是否输出到文件
    bool file_enabled = true;

    // 日志文件路径
    std::string file_path =
        "logs/gateway.log";

    // 日志滚动配置
    LogRotationConfig rotation;
};

} // namespace industrial_gateway::config
