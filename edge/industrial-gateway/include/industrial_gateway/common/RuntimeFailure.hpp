#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace industrial_gateway::common
{
// 在线程间传递首次运行故障。故障不会因后续清理而被覆盖。
// record 可用于异常处理路径：即使错误文本分配失败，也保留故障标志。
class RuntimeFailure final
{
public:
    void record(std::string_view reason, std::string_view detail = {}) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failed_.load()) return;
            reason_.assign(reason);
            if (!detail.empty())
            {
                reason_.append(": ");
                reason_.append(detail);
            }
            failed_.store(true);
        }
        catch (...) { failed_.store(true); }
    }

    bool hasFailure() const noexcept { return failed_.load(); }

    std::string reason() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failed_.load()) return {};
        return reason_.empty() ? "运行故障（详细原因未能保存）" : reason_;
    }

    // 仅在旧线程已回收、新线程尚未启动时调用。
    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reason_.clear();
        failed_.store(false);
    }

private:
    mutable std::mutex mutex_;
    std::atomic<bool> failed_{false};
    std::string reason_;
};
} // namespace industrial_gateway::common
