
#include "YamlConfigLoader.hpp"
#include "LoggerConfigParser.hpp"
#include "ConfigValidator.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <string>


namespace fs = std::filesystem;

namespace config = industrial_gateway::config;
namespace infrastructure_config =
    industrial_gateway::infrastructure::config;


// ==================================================
// 测试夹具
// ==================================================

class LoggerConfigTest : public ::testing::Test
{
protected:

    // YAML 测试文件目录
    fs::path fixtureDirectory() const
    {
        return fs::path(LOGGER_CONFIG_FIXTURE_DIR);
    }


    // 获取测试文件完整路径
    fs::path fixturePath(const std::string& filename) const
    {
        return fixtureDirectory() / filename;
    }


    // --------------------------------------------------
    // 读取 YAML 文件
    // --------------------------------------------------

    bool loadFixture(
        const std::string& filename,
        YAML::Node& root
    )
    {
        error_message.clear();

        return loader.loadFile(
            fixturePath(filename),
            root,
            error_message
        );
    }


    // --------------------------------------------------
    // 读取并解析 YAML 文件
    // --------------------------------------------------

    bool parseFixture(
        const std::string& filename,
        config::LoggerConfig& logger_config
    )
    {
        YAML::Node root;

        if (!loadFixture(filename, root))
        {
            return false;
        }

        return parser.parse(
            root,
            logger_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 读取、解析并校验 YAML 文件
    // --------------------------------------------------

    bool validateFixture(
        const std::string& filename,
        config::LoggerConfig& logger_config
    )
    {
        if (!parseFixture(filename, logger_config))
        {
            return false;
        }

        return validator.validateLoggerConfig(
            logger_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 断言：配置解析失败
    // --------------------------------------------------

    void expectParseFailure(const std::string& filename)
    {
        config::LoggerConfig logger_config;

        // 确保 YAML 文件本身可以正常读取
        YAML::Node root;

        ASSERT_TRUE(loadFixture(filename, root))
            << "YAML 文件读取失败: "
            << error_message;

        error_message.clear();

        // Parser 必须失败
        EXPECT_FALSE(
            parser.parse(
                root,
                logger_config,
                error_message
            )
        ) << "预期解析失败，但实际成功: " << filename;

        EXPECT_FALSE(error_message.empty())
            << "解析失败时没有返回错误信息: " << filename;
    }


    // --------------------------------------------------
    // 断言：配置校验失败
    // --------------------------------------------------

    void expectValidationFailure(const std::string& filename)
    {
        config::LoggerConfig logger_config;

        // 首先必须解析成功
        ASSERT_TRUE(
            parseFixture(filename, logger_config)
        ) << "配置解析失败: " << error_message;

        error_message.clear();

        // Validator 必须失败
        EXPECT_FALSE(
            validator.validateLoggerConfig(
                logger_config,
                error_message
            )
        ) << "预期校验失败，但实际成功: " << filename;

        EXPECT_FALSE(error_message.empty())
            << "校验失败时没有返回错误信息: " << filename;
    }


    // --------------------------------------------------
    // 断言：完整配置流程成功
    // --------------------------------------------------

    void expectValidFixture(const std::string& filename)
    {
        config::LoggerConfig logger_config;

        ASSERT_TRUE(
            validateFixture(filename, logger_config)
        ) << "配置验证失败: " << error_message;
    }


protected:

    // YAML 加载器
    infrastructure_config::YamlConfigLoader loader;

    // 日志配置解析器
    infrastructure_config::LoggerConfigParser parser;

    // 配置校验器
    infrastructure_config::ConfigValidator validator;

    // 错误信息
    std::string error_message;
};


// ==================================================
// 第一组：YamlConfigLoader 测试
// ==================================================


// 正常读取 YAML 文件
TEST_F(LoggerConfigTest, LoadValidFile)
{
    YAML::Node root;

    ASSERT_TRUE(loadFixture("valid_full.yaml", root))
        << error_message;

    EXPECT_TRUE(root.IsMap());

    EXPECT_TRUE(root["logging"].IsMap());
}


// 文件不存在
TEST_F(LoggerConfigTest, LoadNonexistentFile)
{
    YAML::Node root;

    EXPECT_FALSE(
        loader.loadFile(
            fixturePath("__nonexistent__.yaml"),
            root,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// YAML 语法错误
TEST_F(LoggerConfigTest, LoadInvalidSyntax)
{
    YAML::Node root;

    EXPECT_FALSE(
        loadFixture("invalid_syntax.yaml", root)
    );

    EXPECT_FALSE(error_message.empty());
}


// 空 YAML 文件
TEST_F(LoggerConfigTest, LoadEmptyFile)
{
    YAML::Node root;

    ASSERT_TRUE(loadFixture("empty.yaml", root))
        << error_message;

    EXPECT_FALSE(root.IsMap());
}


// 空文件路径
TEST_F(LoggerConfigTest, LoadEmptyPath)
{
    YAML::Node root;

    EXPECT_FALSE(
        loader.loadFile(
            fs::path{},
            root,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// ==================================================
// 第二组：正常日志配置测试
// ==================================================


// 完整日志配置
TEST_F(LoggerConfigTest, ParseFullConfig)
{
    config::LoggerConfig logger_config;

    ASSERT_TRUE(
        validateFixture("valid_full.yaml", logger_config)
    ) << error_message;

    EXPECT_EQ(
        logger_config.level,
        industrial_gateway::common::LogLevel::Debug
    );

    EXPECT_TRUE(logger_config.console_enabled);

    EXPECT_TRUE(logger_config.file_enabled);

    EXPECT_EQ(
        logger_config.file_path,
        "logs/test.log"
    );

    EXPECT_EQ(
        logger_config.rotation.strategy,
        config::LogRotationStrategy::Size
    );

    EXPECT_EQ(
        logger_config.rotation.max_size_bytes,
        10485760ULL
    );

    EXPECT_EQ(
        logger_config.rotation.max_files,
        5U
    );
}


// 最小配置及默认值
TEST_F(LoggerConfigTest, ParseMinimalConfig)
{
    config::LoggerConfig logger_config;

    ASSERT_TRUE(
        validateFixture("valid_minimal.yaml", logger_config)
    ) << error_message;

    EXPECT_EQ(
        logger_config.level,
        industrial_gateway::common::LogLevel::Info
    );

    EXPECT_TRUE(logger_config.console_enabled);

    EXPECT_TRUE(logger_config.file_enabled);

    EXPECT_EQ(
        logger_config.file_path,
        "logs/gateway.log"
    );

    EXPECT_EQ(
        logger_config.rotation.strategy,
        config::LogRotationStrategy::Size
    );

    EXPECT_EQ(
        logger_config.rotation.max_size_bytes,
        10ULL * 1024ULL * 1024ULL
    );

    EXPECT_EQ(
        logger_config.rotation.max_files,
        5U
    );
}


// logging 节点缺失
TEST_F(LoggerConfigTest, ParseMissingLogging)
{
    expectValidFixture("missing_logging.yaml");
}


// 最小合法边界
TEST_F(LoggerConfigTest, ValidateMinimumBoundaries)
{
    expectValidFixture("valid_min_boundaries.yaml");
}


// 单个文件最大容量边界：2 GiB
TEST_F(LoggerConfigTest, ValidateMaximumFileSizeBoundary)
{
    expectValidFixture("valid_max_size_boundary.yaml");
}


// 历史文件数量最大边界：49
TEST_F(LoggerConfigTest, ValidateMaximumFileCountBoundary)
{
    expectValidFixture("valid_max_files_boundary.yaml");
}


// ==================================================
// 第三组：Parser 异常测试
// ==================================================

struct FixtureCase
{
    const char* test_name;
    const char* filename;
};


class LoggerConfigParserFailureTest
    : public LoggerConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    LoggerConfigParserFailureTest,
    RejectInvalidConfiguration
)
{
    expectParseFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidParserConfigurations,
    LoggerConfigParserFailureTest,
    ::testing::Values(

        FixtureCase{"EmptyYaml", "empty.yaml"},

        FixtureCase{"RootNotMap", "root_not_map.yaml"},
        FixtureCase{"LoggingNotMap", "logging_not_map.yaml"},
        FixtureCase{"UnknownLoggingKey", "unknown_logging_key.yaml"},

        FixtureCase{"LevelWrongType", "level_wrong_type.yaml"},
        FixtureCase{"LevelInvalidValue", "level_invalid_value.yaml"},

        FixtureCase{"ConsoleEnabledWrongType", "console_enabled_wrong_type.yaml"},
        FixtureCase{"ConsoleEnabledInvalidValue", "console_enabled_invalid_value.yaml"},

        FixtureCase{"FileEnabledWrongType", "file_enabled_wrong_type.yaml"},
        FixtureCase{"FileEnabledInvalidValue", "file_enabled_invalid_value.yaml"},

        FixtureCase{"FilePathWrongType", "file_path_wrong_type.yaml"},

        FixtureCase{"RotationNotMap", "rotation_not_map.yaml"},
        FixtureCase{"UnknownRotationKey", "unknown_rotation_key.yaml"},

        FixtureCase{"StrategyWrongType", "strategy_wrong_type.yaml"},
        FixtureCase{"StrategyInvalidValue", "strategy_invalid_value.yaml"},

        FixtureCase{"MaxSizeWrongType", "max_size_wrong_type.yaml"},
        FixtureCase{"MaxSizeInvalidValue", "max_size_invalid_value.yaml"},
        FixtureCase{"MaxSizeFractional", "max_size_fractional.yaml"},
        FixtureCase{"MaxSizeNegative", "max_size_negative.yaml"},
        FixtureCase{"MaxSizeOverflow", "max_size_overflow.yaml"},

        FixtureCase{"MaxFilesWrongType", "max_files_wrong_type.yaml"},
        FixtureCase{"MaxFilesInvalidValue", "max_files_invalid_value.yaml"},
        FixtureCase{"MaxFilesFractional", "max_files_fractional.yaml"},
        FixtureCase{"MaxFilesNegative", "max_files_negative.yaml"},
        FixtureCase{"MaxFilesOverflow", "max_files_overflow.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第四组：Validator 异常测试
// ==================================================


class LoggerConfigValidatorFailureTest
    : public LoggerConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    LoggerConfigValidatorFailureTest,
    RejectInvalidConfiguration
)
{
    expectValidationFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidLoggerConfigurations,
    LoggerConfigValidatorFailureTest,
    ::testing::Values(

        FixtureCase{"FilePathEmpty", "file_path_empty.yaml"},

        FixtureCase{"MaxSizeZero", "max_size_zero.yaml"},
        FixtureCase{"MaxSizeOverLimit", "max_size_over_limit.yaml"},

        FixtureCase{"MaxFilesZero", "max_files_zero.yaml"},
        FixtureCase{"MaxFilesOverLimit", "max_files_over_limit.yaml"},

        FixtureCase{"BothOutputsDisabled", "both_outputs_disabled.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第五组：直接校验 C++ 配置对象
// ==================================================


// 非法日志等级
TEST_F(LoggerConfigTest, RejectInvalidLogLevelEnum)
{
    config::LoggerConfig logger_config;

    logger_config.level =
        static_cast<industrial_gateway::common::LogLevel>(999);

    EXPECT_FALSE(
        validator.validateLoggerConfig(
            logger_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// 非法日志滚动策略
TEST_F(LoggerConfigTest, RejectInvalidRotationStrategyEnum)
{
    config::LoggerConfig logger_config;

    logger_config.rotation.strategy =
        static_cast<config::LogRotationStrategy>(999);

    EXPECT_FALSE(
        validator.validateLoggerConfig(
            logger_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}
