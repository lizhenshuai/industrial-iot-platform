
#pragma once

#include "industrial_gateway/common/ILogger.hpp"
#include "industrial_gateway/common/LogLevel.hpp"
#include "industrial_gateway/config/LoggerConfig.hpp"

#include <memory>
#include <string>
#include <string_view>


namespace industrial_gateway::infrastructure::logging
{

// ==================================================
// spdlog 日志实现
// ==================================================

class SpdlogLogger final : public common::ILogger
{
public:

    // ==================================================
    // 构造与析构
    // ==================================================

    SpdlogLogger();

    ~SpdlogLogger() override;


    // ==================================================
    // 禁止拷贝和移动
    // ==================================================

    SpdlogLogger(const SpdlogLogger&) = delete;

    SpdlogLogger& operator=(
        const SpdlogLogger&
    ) = delete;

    SpdlogLogger(SpdlogLogger&&) = delete;

    SpdlogLogger& operator=(
        SpdlogLogger&&
    ) = delete;


    // ==================================================
    // 生命周期管理
    // ==================================================

    // 根据外部传入的配置初始化日志系统
    //
    // config:        已经解析并校验的日志配置
    // error_message: 初始化失败时的错误信息
    //
    // 返回值：
    // true  - 初始化成功
    // false - 初始化失败，传入一个引用对象，通过它来配置，其实也是，这些都不管自己的
    // 提醒关注返回值，不然会报警告
    [[nodiscard]]
    bool initialize(
        const ::industrial_gateway::config::LoggerConfig& config,
        std::string& error_message
    );


    // 关闭日志系统
    void shutdown() noexcept;


    // ==================================================
    // 日志输出
    // ==================================================

    // 实现 ILogger 的核心日志输出接口
    void log(
        common::LogLevel level,
        std::string_view component,
        std::string_view message
    ) override;


    // ==================================================
    // 日志管理
    // ==================================================

    // 动态修改日志等级
    void setLevel(common::LogLevel level) noexcept;


    // 刷新日志缓冲区
    void flush() noexcept;


    // 判断日志系统是否已经初始化
    [[nodiscard]]
    bool isInitialized() const noexcept;


private:

    // ==================================================
    // PImpl：隐藏 spdlog 的具体实现
    // ==================================================

    class Impl;

    std::unique_ptr<Impl> impl_;

};

} // namespace industrial_gateway::infrastructure::logging
