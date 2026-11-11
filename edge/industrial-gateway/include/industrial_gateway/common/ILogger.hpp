
#pragma once
#include "industrial_gateway/common/LogLevel.hpp"
#include <string_view>

// 定义命名空间名字，创建了带层级的嵌套命名空间，industrial_gateway::common::其他地方要使用要带上命名空间前缀
namespace industrial_gateway::common
{

// ==================================================
// 日志等级
// ==================================================




// ==================================================
// 日志抽象接口
// ==================================================

class ILogger
{
public:

    // 析构函数，确保能够正常解析，根据最终实现的来
    virtual ~ILogger() = default;


    // ==================================================
    // 核心日志输出接口
    // ==================================================
    // 运行时多态，声明的哪个，就实现哪个，第一个参数是等级，第二个是哪个模块，第三个是信息，string_view只读，引用读取
    virtual void log(
        LogLevel level,
        std::string_view component,
        std::string_view message
    ) = 0;


    // ==================================================
    // 不同等级的快捷日志接口
    // ==================================================

    void trace(
        std::string_view component,
        std::string_view message
    )
    {
        // 调用,log，定义好内容
        log(LogLevel::Trace, component, message);
    }


    void debug(
        std::string_view component,
        std::string_view message
    )
    {
        log(LogLevel::Debug, component, message);
    }


    void info(
        std::string_view component,
        std::string_view message
    )
    {
        log(LogLevel::Info, component, message);
    }


    void warn(
        std::string_view component,
        std::string_view message
    )
    {
        log(LogLevel::Warn, component, message);
    }


    void error(
        std::string_view component,
        std::string_view message
    )
    {
        log(LogLevel::Error, component, message);
    }


    void critical(
        std::string_view component,
        std::string_view message
    )
    {
        log(LogLevel::Critical, component, message);
    }

};

} // namespace industrial_gateway::common
