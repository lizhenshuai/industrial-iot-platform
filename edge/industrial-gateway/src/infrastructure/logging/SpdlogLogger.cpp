
#include "SpdlogLogger.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>


namespace industrial_gateway::infrastructure::logging
{

namespace
{

// ==================================================
// 日志默认格式
// ==================================================

constexpr char kDefaultLogPattern[] =
    "%Y-%m-%d %H:%M:%S.%e [%^%l%$] [thread %t] %v";


// ==================================================
// 转换日志等级
// ==================================================

spdlog::level::level_enum toSpdlogLevel(
    common::LogLevel level
) noexcept
{
    switch (level)
    {
        case common::LogLevel::Trace:
            return spdlog::level::trace;

        case common::LogLevel::Debug:
            return spdlog::level::debug;

        case common::LogLevel::Info:
            return spdlog::level::info;

        case common::LogLevel::Warn:
            return spdlog::level::warn;

        case common::LogLevel::Error:
            return spdlog::level::err;

        case common::LogLevel::Critical:
            return spdlog::level::critical;

        default:
            return spdlog::level::off;
    }
}

} // namespace


// ==================================================
// PImpl：隐藏 spdlog 的具体实现
// ==================================================

class SpdlogLogger::Impl
{
public:

    // 保护日志对象的生命周期和并发访问
    //
    // 共享锁：
    // 允许多个日志线程同时输出
    //
    // 独占锁：
    // 用于初始化、关闭、修改等级和刷新
    mutable std::shared_mutex lifecycle_mutex;

    // spdlog 日志对象
    //
    // 由 Impl 独占管理，不再需要 shared_ptr
    std::unique_ptr<spdlog::logger> logger;
};


// ==================================================
// 构造与析构
// ==================================================

SpdlogLogger::SpdlogLogger()
    : impl_(std::make_unique<Impl>())
{
}


SpdlogLogger::~SpdlogLogger()
{
    shutdown();
}


// ==================================================
// 初始化日志系统
// ==================================================

bool SpdlogLogger::initialize(
    const ::industrial_gateway::config::LoggerConfig& config,
    std::string& error_message
)
{
    // 清理错误信息
    error_message.clear();

    try
    {
        // 独占锁：
        // 初始化期间不允许其他线程访问日志对象
        //
        // 同时保证 initialize 和 shutdown 串行执行
        std::unique_lock<std::shared_mutex> lock(
            impl_->lifecycle_mutex
        );

        // 禁止重复初始化
        if (impl_->logger)
        {
            error_message = "日志系统已经初始化";
            return false;
        }


        // ==================================================
        // 1. 检查日志输出方式
        // ==================================================

        if (!config.console_enabled && !config.file_enabled)
        {
            error_message =
                "日志输出至少需要启用一种方式";

            return false;
        }


        // ==================================================
        // 2. 转换日志等级
        // ==================================================

        const auto log_level =
            toSpdlogLevel(config.level);

        if (log_level == spdlog::level::off)
        {
            error_message =
                "logging.level 不合法";

            return false;
        }


        // ==================================================
        // 3. 创建日志 Sink
        // ==================================================

        // 保存所有日志输出 Sink
        std::vector<spdlog::sink_ptr> sinks;


        // ----------------------------------------------
        // 3.1 创建控制台 Sink
        // ----------------------------------------------

        if (config.console_enabled)
        {
            auto console_sink =
                std::make_shared<
                    spdlog::sinks::stdout_color_sink_mt
                >();

            sinks.push_back(console_sink);
        }


        // ----------------------------------------------
        // 3.2 创建文件 Sink
        // ----------------------------------------------

        if (config.file_enabled)
        {
            // 文件路径不能为空
            if (config.file_path.empty())
            {
                error_message =
                    "logging.file_path 不能为空";

                return false;
            }


            // 文件大小必须大于 0
            if (config.rotation.max_size_bytes == 0)
            {
                error_message =
                    "logging.rotation.max_size_bytes 必须大于 0";

                return false;
            }


            // 历史文件数量必须大于 0
            if (config.rotation.max_files == 0)
            {
                error_message =
                    "logging.rotation.max_files 必须大于 0";

                return false;
            }


            // ----------------------------------------------
            // 3.2.1 创建日志目录
            // ----------------------------------------------

            const std::filesystem::path log_path(
                config.file_path
            );

            const std::filesystem::path parent_path =
                log_path.parent_path();

            if (!parent_path.empty())
            {
                std::error_code ec;

                std::filesystem::create_directories(
                    parent_path,
                    ec
                );

                if (ec)
                {
                    error_message =
                        "创建日志目录失败: " + ec.message();

                    return false;
                }
            }


            // ----------------------------------------------
            // 3.2.2 根据滚动策略创建文件 Sink
            // ----------------------------------------------

            switch (config.rotation.strategy)
            {
                case ::industrial_gateway::config::
                    LogRotationStrategy::Size:
                {
                    auto file_sink =
                        std::make_shared<
                            spdlog::sinks::rotating_file_sink_mt
                        >(
                            config.file_path,
                            config.rotation.max_size_bytes,
                            config.rotation.max_files
                        );

                    sinks.push_back(file_sink);

                    break;
                }

                default:
                {
                    error_message =
                        "logging.rotation.strategy 不合法";

                    return false;
                }
            }
        }


        // ==================================================
        // 4. 创建 spdlog Logger
        // ==================================================

        auto candidate_logger =
            std::make_unique<spdlog::logger>(
                "industrial_gateway",
                sinks.begin(),
                sinks.end()
            );


        // ==================================================
        // 5. 设置日志等级
        // ==================================================

        candidate_logger->set_level(log_level);


        // ==================================================
        // 6. 设置日志格式
        // ==================================================

        candidate_logger->set_pattern(
            kDefaultLogPattern
        );


        // ==================================================
        // 7. 发布初始化完成的日志对象
        // ==================================================

        // 独占锁保护下，直接提交初始化完成的对象
        impl_->logger = std::move(candidate_logger);

        return true;
    }
    catch (const std::exception& e)
    {
        error_message =
            std::string("日志初始化失败: ") + e.what();

        return false;
    }
    catch (...)
    {
        error_message =
            "日志初始化失败: 未知异常";

        return false;
    }
}


// ==================================================
// 关闭日志系统
// ==================================================

void SpdlogLogger::shutdown() noexcept
{
    try
    {
        // 独占锁：
        // 1. 等待所有正在执行的 log() 完成
        // 2. 阻止新的 log() 进入
        // 3. 保证关闭期间没有其他线程操作日志对象
        std::unique_lock<std::shared_mutex> lock(
            impl_->lifecycle_mutex
        );


        // 尚未初始化，直接返回
        if (!impl_->logger)
        {
            return;
        }


        // 移除日志对象的所有权
        auto logger = std::move(impl_->logger);


        // 关闭前刷新日志
        //
        // logger 在当前作用域结束时销毁
        // 独占锁在 logger 销毁后释放
        logger->flush();
    }
    catch (...)
    {
        // shutdown 为 noexcept 接口
        // 不允许异常向外传播
    }
}


// ==================================================
// 日志输出
// ==================================================

void SpdlogLogger::log(
    common::LogLevel level,
    std::string_view component,
    std::string_view message
)
{
    // 共享锁：
    // 允许多个线程同时执行日志输出
    //
    // 锁需要覆盖整个日志输出过程，
    // 确保 shutdown() 不会提前销毁日志对象
    std::shared_lock<std::shared_mutex> lock(
        impl_->lifecycle_mutex
    );


    // 尚未初始化或已经关闭
    if (!impl_->logger)
    {
        return;
    }


    // ==================================================
    // 1. 转换日志等级
    // ==================================================

    const auto spdlog_level =
        toSpdlogLevel(level);


    // 无效等级不输出
    if (spdlog_level == spdlog::level::off)
    {
        return;
    }


    // ==================================================
    // 2. 日志等级过滤
    // ==================================================

    if (!impl_->logger->should_log(spdlog_level))
    {
        return;
    }


    // ==================================================
    // 3. 转换字符串视图
    // ==================================================

    // 将非拥有型字符串视图转换成拥有型字符串
    const std::string component_text(component);

    const std::string message_text(message);


    // ==================================================
    // 4. 输出日志
    // ==================================================

    impl_->logger->log(
        spdlog_level,
        "[{}] {}",
        component_text,
        message_text
    );
}


// ==================================================
// 动态修改日志等级
// ==================================================

void SpdlogLogger::setLevel(
    common::LogLevel level
) noexcept
{
    try
    {
        // 独占锁：
        // 等待当前正在执行的日志输出完成
        //
        // 保证日志等级修改前后不会与 log() 交叉执行
        std::unique_lock<std::shared_mutex> lock(
            impl_->lifecycle_mutex
        );


        // 日志系统尚未初始化
        if (!impl_->logger)
        {
            return;
        }


        // 转换日志等级
        const auto spdlog_level =
            toSpdlogLevel(level);


        // 非法等级不修改当前日志等级
        if (spdlog_level == spdlog::level::off)
        {
            return;
        }


        // 修改日志等级
        impl_->logger->set_level(spdlog_level);
    }
    catch (...)
    {
        // setLevel 为 noexcept 接口
        // 不允许异常向外传播
    }
}


// ==================================================
// 刷新日志缓冲区
// ==================================================

void SpdlogLogger::flush() noexcept
{
    try
    {
        // 独占锁：
        // 等待当前日志输出完成
        //
        // 刷新期间阻止新的日志写入
        std::unique_lock<std::shared_mutex> lock(
            impl_->lifecycle_mutex
        );


        if (impl_->logger)
        {
            impl_->logger->flush();
        }
    }
    catch (...)
    {
        // 不允许异常向外传播
    }
}


// ==================================================
// 判断日志系统是否已经初始化
// ==================================================

bool SpdlogLogger::isInitialized() const noexcept
{
    try
    {
        // 共享锁：
        // 允许多个线程同时查询初始化状态
        std::shared_lock<std::shared_mutex> lock(
            impl_->lifecycle_mutex
        );

        return static_cast<bool>(
            impl_->logger
        );
    }
    catch (...)
    {
        // 查询失败时返回 false
        return false;
    }
}


} // namespace industrial_gateway::infrastructure::logging
