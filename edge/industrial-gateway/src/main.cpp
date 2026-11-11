#include "bootstrap/GatewayRuntime.hpp"
#include "infrastructure/config/ConfigValidator.hpp"
#include "infrastructure/config/GatewayConfigParser.hpp"
#include "infrastructure/config/YamlConfigLoader.hpp"
#include "infrastructure/logging/SpdlogLogger.hpp"

#include <yaml-cpp/yaml.h>

#include <pthread.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc > 2)
    {
        std::cerr << "用法: industrial_gateway [网关 YAML 路径，默认 config/gateway.yaml]\n";
        return EXIT_FAILURE;
    }

    // 在创建任何后台线程前阻塞退出信号，子线程继承屏蔽集。
    // 主线程同步接收信号并收尾，不在异步信号处理器里调用日志、锁或 join。
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGINT);
    ::sigaddset(&signals, SIGTERM);
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (mask_error != 0)
    {
        std::cerr << "设置退出信号失败: " << std::strerror(mask_error) << '\n';
        return EXIT_FAILURE;
    }

    try
    {
        using namespace industrial_gateway;
        YAML::Node root;
        config::GatewayConfig config;
        std::string error;
        const std::string config_path = argc == 2 ? argv[1] : "config/gateway.yaml";
        if (!infrastructure::config::YamlConfigLoader{}.loadFile(config_path, root, error) ||
            !infrastructure::config::GatewayConfigParser{}.parse(root, config, error) ||
            !infrastructure::config::ConfigValidator{}.validateGatewayConfig(config, error))
        {
            std::cerr << "加载网关配置失败: " << error << '\n';
            return EXIT_FAILURE;
        }
        // 日志与数据库路径沿用配置原值，相对路径以工作目录为基准。
        infrastructure::logging::SpdlogLogger logger;
        if (!logger.initialize(config.logging, error))
        {
            std::cerr << "初始化日志失败: " << error << '\n';
            return EXIT_FAILURE;
        }
        // runtime 先析构，确保所有线程结束后才销毁 logger。
        bootstrap::GatewayRuntime runtime(config, logger);
        if (!runtime.start(error))
        {
            std::cerr << "启动网关失败: " << error << '\n';
            return EXIT_FAILURE;
        }

        int result = EXIT_SUCCESS;
        using Clock = std::chrono::steady_clock;
        constexpr std::chrono::seconds health_interval{10};
        auto next_check = Clock::now() + health_interval;
        while (true)
        {
            // 直接等待退出信号直到下一次健康检查；不每 200ms 唤醒。
            // 使用单调时钟截止时间，EINTR 不会把检查时间不断向后推迟。
            const auto now = Clock::now();
            const auto remaining = next_check > now
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(next_check - now)
                : std::chrono::nanoseconds::zero();
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
            const timespec wait{static_cast<time_t>(seconds.count()),
                static_cast<long>((remaining - seconds).count())};
            const int signal = ::sigtimedwait(&signals, nullptr, &wait);
            if (signal == SIGINT || signal == SIGTERM) break;
            if (signal < 0 && errno != EAGAIN && errno != EINTR)
            {
                std::cerr << "等待退出信号失败: " << std::strerror(errno) << '\n';
                result = EXIT_FAILURE;
                break;
            }
            if (Clock::now() < next_check) continue;
            next_check = Clock::now() + health_interval;
            if (!runtime.checkHealth(error))
            {
                // 故障先记录再关闭；关闭本身仍可能等待协议调用或存储重试。
                std::cerr << "网关运行故障，正在停止: " << error << '\n';
                try { logger.critical("main", error); }
                catch (...) {} // 日志失败不应阻止故障退出。
                result = EXIT_FAILURE;
                break;
            }
        }
        runtime.stop();
        logger.flush();
        return result;
    }
    catch (const std::exception& error)
    {
        std::cerr << "网关运行失败: " << error.what() << '\n';
    }
    catch (...) { std::cerr << "网关运行发生未知异常\n"; }
    return EXIT_FAILURE;
}
