#pragma once

namespace industrial_gateway::common
{

// ==================================================
// 日志等级
// ==================================================

enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

} // namespace industrial_gateway::common