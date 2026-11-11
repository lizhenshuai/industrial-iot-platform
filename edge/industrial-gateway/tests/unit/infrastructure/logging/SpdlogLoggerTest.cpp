
#include "SpdlogLogger.hpp"

#include "YamlConfigLoader.hpp"
#include "LoggerConfigParser.hpp"
#include "ConfigValidator.hpp"

#include "industrial_gateway/config/LoggerConfig.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// Linux 标准输出捕获
#include <unistd.h>


namespace fs = std::filesystem;

namespace industrial_gateway::tests::logging
{

using industrial_gateway::common::LogLevel;
using industrial_gateway::config::LoggerConfig;

using industrial_gateway::infrastructure::logging::SpdlogLogger;

using industrial_gateway::infrastructure::config::YamlConfigLoader;
using industrial_gateway::infrastructure::config::LoggerConfigParser;
using industrial_gateway::infrastructure::config::ConfigValidator;


// ==================================================
// 标准输出捕获工具
// ==================================================

class StdoutCapture final
{
public:

    StdoutCapture() = default;

    ~StdoutCapture()
    {
        if (active_)
        {
            stop();
        }

        if (read_fd_ >= 0)
        {
            ::close(read_fd_);
        }

        if (saved_stdout_ >= 0)
        {
            ::close(saved_stdout_);
        }
    }


    // 禁止拷贝
    StdoutCapture(const StdoutCapture&) = delete;

    StdoutCapture& operator=(const StdoutCapture&) = delete;


    // 开始捕获标准输出
    bool start(std::string& error_message)
    {
        error_message.clear();

        std::fflush(stdout);

        int pipe_fds[2] = {-1, -1};

        if (::pipe(pipe_fds) != 0)
        {
            error_message =
                std::string("创建输出管道失败: ") +
                std::strerror(errno);

            return false;
        }

        saved_stdout_ = ::dup(STDOUT_FILENO);

        if (saved_stdout_ < 0)
        {
            error_message =
                std::string("复制标准输出失败: ") +
                std::strerror(errno);

            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);

            return false;
        }

        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0)
        {
            error_message =
                std::string("重定向标准输出失败: ") +
                std::strerror(errno);

            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            ::close(saved_stdout_);

            saved_stdout_ = -1;

            return false;
        }

        ::close(pipe_fds[1]);

        read_fd_ = pipe_fds[0];

        active_ = true;

        return true;
    }


    // 停止捕获并返回捕获的内容
    std::string stop()
    {
        if (!active_)
        {
            return {};
        }

        std::fflush(stdout);

        // 恢复原来的标准输出
        ::dup2(saved_stdout_, STDOUT_FILENO);

        ::close(saved_stdout_);

        saved_stdout_ = -1;

        active_ = false;


        // 读取管道内容
        std::string output;

        char buffer[4096];

        while (true)
        {
            const ssize_t bytes = ::read(
                read_fd_,
                buffer,
                sizeof(buffer)
            );

            if (bytes > 0)
            {
                output.append(
                    buffer,
                    static_cast<std::size_t>(bytes)
                );

                continue;
            }

            if (bytes < 0 && errno == EINTR)
            {
                continue;
            }

            break;
        }

        ::close(read_fd_);

        read_fd_ = -1;

        return output;
    }


private:

    int saved_stdout_ = -1;

    int read_fd_ = -1;

    bool active_ = false;
};


// ==================================================
// 日志测试 Fixture
// ==================================================

class LoggingTest : public ::testing::Test
{
protected:

    // 日志对象
    SpdlogLogger logger;


    // 配置模块对象
    YamlConfigLoader loader;

    LoggerConfigParser parser;

    ConfigValidator validator;


    // 测试目录
    fs::path project_root;

    fs::path test_directory;

    fs::path config_directory;

    // 所有测试输出的根目录
    fs::path output_directory;

    // 当前测试用例独占的输出目录
    fs::path test_output_directory;


    // ==================================================
    // 测试初始化
    // ==================================================

    void SetUp() override
    {
        std::error_code ec;

        // 优先从当前测试源文件的位置寻找项目根目录。
        // CMake 通常会将 __FILE__ 展开为源文件的绝对路径。
        fs::path source_path(__FILE__);

        if (source_path.is_relative())
        {
            source_path = fs::absolute(source_path, ec);

            ASSERT_FALSE(ec)
                << "解析测试源文件路径失败: " << ec.message();
        }

        project_root = findProjectRoot(source_path.parent_path());

        // 兼容某些构建方式下 __FILE__ 为相对路径的情况。
        // 这里只读取当前工作目录，不修改进程级工作目录。
        if (project_root.empty())
        {
            const fs::path current_directory = fs::current_path(ec);

            ASSERT_FALSE(ec)
                << "获取当前工作目录失败: " << ec.message();

            project_root = findProjectRoot(current_directory);
        }

        ASSERT_FALSE(project_root.empty())
            << "无法找到项目根目录，请检查测试源文件路径和项目目录结构";

        project_root = fs::weakly_canonical(project_root, ec);

        ASSERT_FALSE(ec)
            << "解析项目根目录失败: " << ec.message();


        test_directory =
            project_root / "tests/unit/infrastructure/logging";

        config_directory =
            test_directory / "configs";

        output_directory =
            test_directory / "output";


        const bool config_directory_exists =
            fs::is_directory(config_directory, ec);

        ASSERT_FALSE(ec)
            << "检查测试配置目录失败: " << ec.message();

        ASSERT_TRUE(config_directory_exists)
            << "测试配置目录不存在: "
            << config_directory.string();


        fs::create_directories(output_directory, ec);

        ASSERT_FALSE(ec)
            << "创建测试日志目录失败: "
            << ec.message();

        ASSERT_TRUE(fs::is_directory(output_directory, ec))
            << "测试日志输出路径不是目录: "
            << output_directory.string();

        ASSERT_FALSE(ec)
            << "检查测试日志目录失败: " << ec.message();


        // 每个测试用例使用独立目录；PID 进一步隔离并行运行的测试进程。
        const ::testing::TestInfo* test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();

        ASSERT_NE(test_info, nullptr)
            << "无法获取当前 GoogleTest 用例信息";

        const std::string process_directory =
            "process_" + std::to_string(static_cast<long long>(::getpid()));

        test_output_directory =
            output_directory /
            process_directory /
            test_info->test_suite_name() /
            test_info->name();

        fs::create_directories(test_output_directory, ec);

        ASSERT_FALSE(ec)
            << "创建当前测试用例输出目录失败: "
            << ec.message();
    }


    // ==================================================
    // 测试结束
    // ==================================================

    void TearDown() override
    {
        // 确保日志系统关闭。
        // 有意保留本次测试生成的日志，便于失败后排查。
        logger.shutdown();
    }


    // ==================================================
    // 寻找项目根目录
    // ==================================================

    static fs::path findProjectRoot(
        const fs::path& start_directory
    )
    {
        fs::path current = start_directory;
        std::error_code ec;

        while (!current.empty())
        {
            const fs::path config_path =
                current /
                "tests/unit/infrastructure/logging/configs";

            ec.clear();

            if (fs::is_directory(config_path, ec) && !ec)
            {
                return current;
            }

            const fs::path parent = current.parent_path();

            if (parent == current)
            {
                break;
            }

            current = parent;
        }

        return {};
    }


    // ==================================================
    // 读取、解析、校验 YAML 配置
    // ==================================================

    bool loadValidatedConfig(
        const std::string& file_name,
        LoggerConfig& config,
        std::string& error_message
    )
    {
        YAML::Node root;


        // 1. 加载 YAML 文件
        if (!loader.loadFile(
                config_directory / file_name,
                root,
                error_message))
        {
            return false;
        }


        // 2. 解析日志配置
        if (!parser.parse(
                root,
                config,
                error_message))
        {
            return false;
        }


        // 3. 校验日志配置
        if (!validator.validateLoggerConfig(
                config,
                error_message))
        {
            return false;
        }

        // 配置文件中的原始路径已经完成解析和校验。
        // 实际执行测试时，将日志文件重定向到当前测试用例独占的目录，
        // 防止不同测试复用同一个日志文件而相互污染。
        const fs::path configured_path(config.file_path);
        const fs::path log_filename = configured_path.filename();

        if (log_filename.empty() ||
            log_filename == "." ||
            log_filename == "..")
        {
            error_message = "日志文件路径必须包含有效的文件名";
            return false;
        }

        config.file_path =
            (test_output_directory / log_filename).string();

        return true;
    }


    // ==================================================
    // 获取实际日志文件路径
    // ==================================================

    fs::path resolveLogPath(
        const LoggerConfig& config
    ) const
    {
        fs::path path(config.file_path);

        if (path.is_relative())
        {
            path = project_root / path;
        }

        return path.lexically_normal();
    }


    // ==================================================
    // 判断 child 是否为 parent 的严格子路径（按路径组件比较）
    // ==================================================

    static bool isStrictChildPath(
        const fs::path& parent,
        const fs::path& child
    )
    {
        auto parent_it = parent.begin();
        auto child_it = child.begin();

        for (; parent_it != parent.end(); ++parent_it, ++child_it)
        {
            if (child_it == child.end() || *parent_it != *child_it)
            {
                return false;
            }
        }

        return child_it != child.end();
    }


    // ==================================================
    // 检查路径是否严格位于当前测试用例的输出目录内
    // ==================================================

    bool isSafeOutputPath(
        const fs::path& path,
        std::string& error_message
    ) const
    {
        error_message.clear();

        if (path.empty())
        {
            error_message = "日志文件路径不能为空";
            return false;
        }

        std::error_code ec;

        // 输出根目录必须是实际目录，不能是符号链接。
        const fs::file_status output_status =
            fs::symlink_status(output_directory, ec);

        if (ec)
        {
            error_message =
                "检查测试输出目录失败: " + ec.message();
            return false;
        }

        if (!fs::is_directory(output_status) ||
            fs::is_symlink(output_status))
        {
            error_message =
                "拒绝操作：测试输出路径不是普通目录: " +
                output_directory.string();
            return false;
        }

        // 检查 output_directory 到 test_output_directory 之间的每一级目录，
        // 防止某一级目录被替换成符号链接后指向其他测试目录或外部目录。
        const fs::path relative_test_directory =
            test_output_directory.lexically_relative(output_directory);

        if (relative_test_directory.empty() ||
            relative_test_directory.is_absolute())
        {
            error_message =
                "拒绝操作：当前测试输出目录不在测试输出根目录内";
            return false;
        }

        fs::path current_directory = output_directory;

        for (const auto& component : relative_test_directory)
        {
            if (component == "." || component.empty())
            {
                continue;
            }

            if (component == "..")
            {
                error_message =
                    "拒绝操作：当前测试输出目录包含非法路径组件";
                return false;
            }

            current_directory /= component;

            const fs::file_status directory_status =
                fs::symlink_status(current_directory, ec);

            if (ec)
            {
                error_message =
                    "检查测试用例输出目录失败: " + ec.message();
                return false;
            }

            if (!fs::is_directory(directory_status) ||
                fs::is_symlink(directory_status))
            {
                error_message =
                    "拒绝操作：测试用例输出路径包含非普通目录或符号链接: " +
                    current_directory.string();
                return false;
            }
        }

        // weakly_canonical 会解析已有路径中的符号链接，
        // 同时允许目标日志文件本身尚不存在。
        const fs::path canonical_project_root =
            fs::weakly_canonical(project_root, ec);

        if (ec)
        {
            error_message =
                "解析项目根目录失败: " + ec.message();
            return false;
        }

        const fs::path canonical_output_directory =
            fs::weakly_canonical(output_directory, ec);

        if (ec)
        {
            error_message =
                "解析测试输出根目录失败: " + ec.message();
            return false;
        }

        const fs::path canonical_test_directory =
            fs::weakly_canonical(test_output_directory, ec);

        if (ec)
        {
            error_message =
                "解析当前测试输出目录失败: " + ec.message();
            return false;
        }

        fs::path target_path = path;

        if (target_path.is_relative())
        {
            target_path = project_root / target_path;
        }

        const fs::path canonical_target =
            fs::weakly_canonical(target_path, ec);

        if (ec)
        {
            error_message =
                "解析日志文件路径失败: " + ec.message();
            return false;
        }

        // 输出根目录必须位于项目根目录内。
        if (!isStrictChildPath(
                canonical_project_root,
                canonical_output_directory))
        {
            error_message =
                "拒绝操作：测试输出根目录不在项目根目录内: " +
                output_directory.string();
            return false;
        }

        // 当前测试目录必须位于输出根目录内。
        if (!isStrictChildPath(
                canonical_output_directory,
                canonical_test_directory))
        {
            error_message =
                "拒绝操作：当前测试输出目录不在测试输出根目录内: " +
                test_output_directory.string();
            return false;
        }

        // 日志文件必须是当前测试目录的严格子路径。
        if (!isStrictChildPath(
                canonical_test_directory,
                canonical_target))
        {
            error_message =
                "拒绝操作：日志文件路径不在当前测试用例输出目录内: " +
                path.string();
            return false;
        }

        return true;
    }


    // ==================================================
    // 安全删除单个日志文件
    // ==================================================

    bool removeLogArtifact(
        const fs::path& artifact_path,
        std::string& error_message
    ) const
    {
        if (!isSafeOutputPath(artifact_path, error_message))
        {
            return false;
        }

        std::error_code ec;

        const fs::file_status artifact_status =
            fs::symlink_status(artifact_path, ec);

        // 文件不存在时无需清理。
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && artifact_status.type() == fs::file_type::not_found))
        {
            return true;
        }

        if (ec)
        {
            error_message =
                "检查日志文件失败: " + artifact_path.string() +
                ", 原因: " + ec.message();

            return false;
        }

        // 只允许删除普通文件或符号链接。
        // FIFO、Socket、块设备、字符设备以及目录等一律拒绝。
        if (!fs::is_regular_file(artifact_status) &&
            !fs::is_symlink(artifact_status))
        {
            error_message =
                "拒绝删除非普通文件或符号链接的文件系统对象: " +
                artifact_path.string();

            return false;
        }

        // 使用 remove 而不是 remove_all，绝不递归删除目录内容。
        // 测试输出目录应由当前测试独占，避免其他进程并发替换路径。
        fs::remove(artifact_path, ec);

        if (ec)
        {
            error_message =
                "删除日志文件失败: " + artifact_path.string() +
                ", 原因: " + ec.message();

            return false;
        }

        return true;
    }


    // ==================================================
    // 清理日志文件及历史滚动文件
    // ==================================================

    bool cleanLogArtifacts(
        const fs::path& log_path,
        std::string& error_message
    ) const
    {
        error_message.clear();

        // 删除当前日志文件
        if (!removeLogArtifact(log_path, error_message))
        {
            return false;
        }


        // 删除历史滚动文件
        for (std::size_t i = 1; i <= 50; ++i)
        {
            const fs::path rotated_path =
                log_path.string() + "." + std::to_string(i);

            if (!removeLogArtifact(rotated_path, error_message))
            {
                return false;
            }
        }

        return true;
    }


    // ==================================================
    // 读取日志文件
    // ==================================================

    static std::string readFile(
        const fs::path& file_path
    )
    {
        std::ifstream input(
            file_path,
            std::ios::binary
        );

        if (!input.is_open())
        {
            return {};
        }

        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        );
    }


    // ==================================================
    // 统计字符串出现次数
    // ==================================================

    static std::size_t countOccurrences(
        const std::string& content,
        const std::string& target
    )
    {
        if (target.empty())
        {
            return 0;
        }

        std::size_t count = 0;

        std::size_t position = 0;

        while (true)
        {
            position = content.find(target, position);

            if (position == std::string::npos)
            {
                break;
            }

            ++count;

            position += target.size();
        }

        return count;
    }
};


// ==================================================
// 1. 正常初始化与生命周期测试
// ==================================================

TEST_F(
    LoggingTest,
    BasicInitializationAndLifecycle
)
{
    LoggerConfig config;

    std::string error_message;


    // 读取并校验配置
    ASSERT_TRUE(
        loadValidatedConfig(
            "basic.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    // 默认未初始化
    EXPECT_FALSE(logger.isInitialized());


    // 正常初始化
    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;

    EXPECT_TRUE(logger.isInitialized());


    // 正常输出
    logger.info(
        "Lifecycle",
        "BASIC_INITIALIZED_MARKER"
    );

    logger.flush();


    // 检查日志文件
    ASSERT_TRUE(fs::exists(log_path));

    std::string content = readFile(log_path);

    EXPECT_NE(
        content.find("BASIC_INITIALIZED_MARKER"),
        std::string::npos
    );


    // 重复初始化应该失败
    std::string duplicate_error;

    EXPECT_FALSE(
        logger.initialize(config, duplicate_error)
    );

    EXPECT_FALSE(duplicate_error.empty());

    // 原日志对象仍然有效
    EXPECT_TRUE(logger.isInitialized());


    logger.info(
        "Lifecycle",
        "AFTER_DUPLICATE_INITIALIZE_MARKER"
    );


    // 正常关闭
    logger.shutdown();

    EXPECT_FALSE(logger.isInitialized());


    // 关闭后输出日志，不应该崩溃，也不应该写入
    logger.info(
        "Lifecycle",
        "AFTER_SHUTDOWN_MARKER"
    );


    // 重复关闭
    logger.shutdown();

    EXPECT_FALSE(logger.isInitialized());


    // 关闭后重新初始化
    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;

    EXPECT_TRUE(logger.isInitialized());


    logger.info(
        "Lifecycle",
        "REINITIALIZED_MARKER"
    );

    logger.flush();

    logger.shutdown();


    // 检查最终日志内容
    content = readFile(log_path);

    EXPECT_NE(
        content.find("BASIC_INITIALIZED_MARKER"),
        std::string::npos
    );

    EXPECT_NE(
        content.find("AFTER_DUPLICATE_INITIALIZE_MARKER"),
        std::string::npos
    );

    EXPECT_NE(
        content.find("REINITIALIZED_MARKER"),
        std::string::npos
    );

    EXPECT_EQ(
        content.find("AFTER_SHUTDOWN_MARKER"),
        std::string::npos
    );
}


// ==================================================
// 2. 日志等级过滤与动态修改测试
// ==================================================

TEST_F(
    LoggingTest,
    WarnLevelFilteringAndDynamicLevel
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "warn_level.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    // 当前等级为 Warn
    logger.trace("Test", "LEVEL_TRACE_BEFORE");

    logger.debug("Test", "LEVEL_DEBUG_BEFORE");

    logger.info("Test", "LEVEL_INFO_BEFORE");

    logger.warn("Test", "LEVEL_WARN_BEFORE");

    logger.error("Test", "LEVEL_ERROR_BEFORE");

    logger.critical("Test", "LEVEL_CRITICAL_BEFORE");

    logger.flush();


    std::string content = readFile(log_path);


    // 低于 Warn 的等级应该被过滤
    EXPECT_EQ(
        content.find("LEVEL_TRACE_BEFORE"),
        std::string::npos
    );

    EXPECT_EQ(
        content.find("LEVEL_DEBUG_BEFORE"),
        std::string::npos
    );

    EXPECT_EQ(
        content.find("LEVEL_INFO_BEFORE"),
        std::string::npos
    );


    // Warn 及以上等级正常输出
    EXPECT_NE(
        content.find("LEVEL_WARN_BEFORE"),
        std::string::npos
    );

    EXPECT_NE(
        content.find("LEVEL_ERROR_BEFORE"),
        std::string::npos
    );

    EXPECT_NE(
        content.find("LEVEL_CRITICAL_BEFORE"),
        std::string::npos
    );


    // 动态修改日志等级
    logger.setLevel(LogLevel::Debug);


    logger.trace("Test", "LEVEL_TRACE_AFTER");

    logger.debug("Test", "LEVEL_DEBUG_AFTER");

    logger.info("Test", "LEVEL_INFO_AFTER");

    logger.flush();


    content = readFile(log_path);


    // Debug 等级已经允许输出
    EXPECT_NE(
        content.find("LEVEL_DEBUG_AFTER"),
        std::string::npos
    );


    // Trace 仍然被过滤
    EXPECT_EQ(
        content.find("LEVEL_TRACE_AFTER"),
        std::string::npos
    );


    // Info 也应该输出
    EXPECT_NE(
        content.find("LEVEL_INFO_AFTER"),
        std::string::npos
    );
}


// ==================================================
// 3. 仅控制台输出测试
// ==================================================

TEST_F(
    LoggingTest,
    ConsoleOnlyOutput
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "console_only.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    StdoutCapture capture;

    ASSERT_TRUE(
        capture.start(error_message)
    ) << error_message;


    logger.info(
        "Console",
        "CONSOLE_ONLY_MARKER"
    );

    logger.flush();


    const std::string console_output =
        capture.stop();


    // 控制台应该存在日志
    EXPECT_NE(
        console_output.find("CONSOLE_ONLY_MARKER"),
        std::string::npos
    );


    // 不应该创建日志文件
    EXPECT_FALSE(fs::exists(log_path));
}


// ==================================================
// 4. 仅文件输出测试
// ==================================================

TEST_F(
    LoggingTest,
    FileOnlyOutput
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "file_only.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    StdoutCapture capture;

    ASSERT_TRUE(
        capture.start(error_message)
    ) << error_message;


    logger.info(
        "File",
        "FILE_ONLY_MARKER"
    );

    logger.flush();


    const std::string console_output =
        capture.stop();


    // 不应该输出到控制台
    EXPECT_EQ(
        console_output.find("FILE_ONLY_MARKER"),
        std::string::npos
    );


    // 文件应该存在
    ASSERT_TRUE(fs::exists(log_path));


    const std::string content =
        readFile(log_path);


    EXPECT_NE(
        content.find("FILE_ONLY_MARKER"),
        std::string::npos
    );

    EXPECT_NE(
        content.find("[File]"),
        std::string::npos
    );
}


// ==================================================
// 5. 控制台和文件同时输出测试
// ==================================================

TEST_F(
    LoggingTest,
    ConsoleAndFileOutput
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "console_file.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    StdoutCapture capture;

    ASSERT_TRUE(
        capture.start(error_message)
    ) << error_message;


    logger.info(
        "Both",
        "CONSOLE_AND_FILE_MARKER"
    );

    logger.flush();


    const std::string console_output =
        capture.stop();


    // 控制台存在日志
    EXPECT_NE(
        console_output.find("CONSOLE_AND_FILE_MARKER"),
        std::string::npos
    );


    // 文件存在日志
    ASSERT_TRUE(fs::exists(log_path));

    const std::string file_content =
        readFile(log_path);

    EXPECT_NE(
        file_content.find("CONSOLE_AND_FILE_MARKER"),
        std::string::npos
    );
}


// ==================================================
// 6. 文件滚动与历史文件数量测试
// ==================================================

TEST_F(
    LoggingTest,
    FileRotationAndHistoryLimit
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "rotation.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    // 每条日志约 250 字节
    const std::string payload(200, 'R');


    // 写入足够多的日志触发滚动
    for (int i = 0; i < 100; ++i)
    {
        const std::string message =
            "ROTATION_MARKER_" +
            std::to_string(i) +
            "_" +
            payload;

        logger.info("Rotation", message);
    }


    logger.flush();

    // 关闭后再检查文件
    logger.shutdown();


    // 当前日志文件必须存在
    ASSERT_TRUE(fs::exists(log_path));


    // 统计历史文件数量
    std::size_t rotated_count = 0;

    for (std::size_t i = 1; i <= 50; ++i)
    {
        // spdlog 的命名格式：
        // rotation.log
        // rotation.1.log
        // rotation.2.log

        const fs::path rotated_path =
            log_path.parent_path() /
            (
                log_path.stem().string() +
                "." +
                std::to_string(i) +
                log_path.extension().string()
            );

        if (fs::exists(rotated_path))
        {
            ++rotated_count;
        }
    }


    // 必须发生过滚动
    EXPECT_GT(rotated_count, 0U);


    // 历史文件不能超过配置数量
    EXPECT_LE(
        rotated_count,
        config.rotation.max_files
    );


    // 当前文件 + 历史文件不能超过三个
    EXPECT_LE(
        rotated_count + 1,
        3U
    );
}


// ==================================================
// 7. 非法配置拦截测试
// ==================================================

TEST_F(
    LoggingTest,
    InvalidLoggerConfigRejectedByValidator
)
{
    const fs::path config_path =
        config_directory / "invalid.yaml";


    YAML::Node root;

    std::string error_message;


    // 1. YAML 文件读取应该成功
    ASSERT_TRUE(
        loader.loadFile(
            config_path,
            root,
            error_message
        )
    ) << error_message;


    LoggerConfig config;


    // 2. Parser 应该解析成功
    ASSERT_TRUE(
        parser.parse(
            root,
            config,
            error_message
        )
    ) << error_message;


    // 3. Validator 应该拒绝配置。
    // 本测试只验证配置校验器的职责，不调用 SpdlogLogger::initialize，
    // 因为日志实现层是否重复执行配置校验属于另一项接口契约。
    error_message.clear();

    EXPECT_FALSE(
        validator.validateLoggerConfig(
            config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// ==================================================
// 8. 日志文件创建失败测试
// ==================================================

TEST_F(
    LoggingTest,
    FileCreationFailure
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "file_only.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    // 创建父目录
    std::error_code ec;

    fs::create_directories(
        log_path.parent_path(),
        ec
    );

    ASSERT_FALSE(ec)
        << ec.message();


    // 故意创建一个与日志文件同名的目录
    // 使 spdlog 无法将其作为普通文件打开
    fs::create_directory(log_path, ec);

    ASSERT_FALSE(ec)
        << ec.message();


    // 初始化应该失败
    const bool initialized =
        logger.initialize(config, error_message);


    if (initialized)
    {
        logger.shutdown();
    }


    EXPECT_FALSE(initialized);

    EXPECT_FALSE(error_message.empty());

    EXPECT_FALSE(logger.isInitialized());


    // 清理故意创建的空目录，不进行递归删除
    fs::remove(log_path, ec);

    EXPECT_FALSE(ec)
        << "清理测试目录失败: " << ec.message();
}


// ==================================================
// 9. 多线程日志输出测试
// ==================================================

TEST_F(
    LoggingTest,
    MultiThreadLogging
)
{
    LoggerConfig config;

    std::string error_message;


    ASSERT_TRUE(
        loadValidatedConfig(
            "file_only.yaml",
            config,
            error_message
        )
    ) << error_message;


    const fs::path log_path =
        resolveLogPath(config);

    ASSERT_TRUE(
        cleanLogArtifacts(log_path, error_message)
    ) << error_message;


    ASSERT_TRUE(
        logger.initialize(config, error_message)
    ) << error_message;


    constexpr int kThreadCount = 4;

    constexpr int kMessagesPerThread = 100;


    std::vector<std::thread> workers;

    workers.reserve(kThreadCount);


    // 创建多个工作线程
    for (int thread_index = 0;
         thread_index < kThreadCount;
         ++thread_index)
    {
        workers.emplace_back(
            [this, thread_index]()
            {
                for (int i = 0;
                     i < kMessagesPerThread;
                     ++i)
                {
                    const std::string message =
                        "THREAD_MARKER_" +
                        std::to_string(thread_index) +
                        "_" +
                        std::to_string(i);

                    logger.info(
                        "Worker",
                        message
                    );
                }
            }
        );
    }


    // 等待所有线程结束
    for (auto& worker : workers)
    {
        worker.join();
    }


    logger.flush();

    logger.shutdown();


    // 读取最终日志文件
    ASSERT_TRUE(fs::exists(log_path));

    const std::string content =
        readFile(log_path);


    // 统计日志数量
    const std::size_t message_count =
        countOccurrences(
            content,
            "THREAD_MARKER_"
        );


    // 4 个线程，每个线程 100 条
    EXPECT_EQ(
        message_count,
        static_cast<std::size_t>(
            kThreadCount * kMessagesPerThread
        )
    );
}

} // namespace industrial_gateway::tests::logging
