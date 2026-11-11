
#include "YamlConfigLoader.hpp"
#include "StorageConfigParser.hpp"
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

class StorageConfigTest : public ::testing::Test
{
protected:

    // YAML 测试文件目录
    fs::path fixtureDirectory() const
    {
        return fs::path(STORAGE_CONFIG_FIXTURE_DIR);
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
        config::StorageConfig& storage_config
    )
    {
        YAML::Node root;

        if (!loadFixture(filename, root))
        {
            return false;
        }

        return parser.parse(
            root,
            storage_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 读取、解析并校验 YAML 文件
    // --------------------------------------------------

    bool validateFixture(
        const std::string& filename,
        config::StorageConfig& storage_config
    )
    {
        if (!parseFixture(filename, storage_config))
        {
            return false;
        }

        return validator.validateStorageConfig(
            storage_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 断言：配置解析失败
    // --------------------------------------------------

    void expectParseFailure(const std::string& filename)
    {
        config::StorageConfig storage_config;

        YAML::Node root;

        ASSERT_TRUE(loadFixture(filename, root))
            << "YAML 文件读取失败: "
            << error_message;

        error_message.clear();

        EXPECT_FALSE(
            parser.parse(
                root,
                storage_config,
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
        config::StorageConfig storage_config;

        ASSERT_TRUE(
            parseFixture(filename, storage_config)
        ) << "配置解析失败: " << error_message;

        error_message.clear();

        EXPECT_FALSE(
            validator.validateStorageConfig(
                storage_config,
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
        config::StorageConfig storage_config;

        ASSERT_TRUE(
            validateFixture(filename, storage_config)
        ) << "配置验证失败: " << error_message;
    }


protected:

    // YAML 加载器
    infrastructure_config::YamlConfigLoader loader;

    // SQLite 配置解析器
    infrastructure_config::StorageConfigParser parser;

    // 配置校验器
    infrastructure_config::ConfigValidator validator;

    // 错误信息
    std::string error_message;
};


// ==================================================
// 第一组：正常配置测试
// ==================================================


// 完整 SQLite 配置
TEST_F(StorageConfigTest, ParseFullConfig)
{
    config::StorageConfig storage_config;

    ASSERT_TRUE(
        validateFixture("valid_full.yaml", storage_config)
    ) << error_message;

    EXPECT_EQ(
        storage_config.database_path,
        "data/test_gateway.db"
    );

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        12000
    );
}


// 最小配置及默认值
TEST_F(StorageConfigTest, ParseMinimalConfig)
{
    config::StorageConfig storage_config;

    ASSERT_TRUE(
        validateFixture("valid_minimal.yaml", storage_config)
    ) << error_message;

    EXPECT_EQ(
        storage_config.database_path,
        "data/gateway.db"
    );

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        5000
    );
}


// storage 节点缺失
TEST_F(StorageConfigTest, ParseMissingStorage)
{
    config::StorageConfig storage_config;

    ASSERT_TRUE(
        validateFixture("missing_storage.yaml", storage_config)
    ) << error_message;

    EXPECT_EQ(
        storage_config.database_path,
        "data/gateway.db"
    );

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        5000
    );
}


// ==================================================
// 第二组：Parser 异常测试
// ==================================================

struct FixtureCase
{
    const char* test_name;
    const char* filename;
};


class StorageConfigParserFailureTest
    : public StorageConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    StorageConfigParserFailureTest,
    RejectInvalidConfiguration
)
{
    expectParseFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidParserConfigurations,
    StorageConfigParserFailureTest,
    ::testing::Values(

        FixtureCase{"EmptyYaml", "empty.yaml"},

        FixtureCase{"RootNotMap", "root_not_map.yaml"},

        FixtureCase{"StorageNotMap", "storage_not_map.yaml"},

        FixtureCase{"UnknownStorageKey", "unknown_storage_key.yaml"},

        FixtureCase{"DatabasePathWrongType", "database_path_wrong_type.yaml"},

        FixtureCase{"BusyTimeoutWrongType", "busy_timeout_wrong_type.yaml"},

        FixtureCase{"BusyTimeoutFractional", "busy_timeout_fractional.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第三组：Validator 异常测试
// ==================================================


class StorageConfigValidatorFailureTest
    : public StorageConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    StorageConfigValidatorFailureTest,
    RejectInvalidConfiguration
)
{
    expectValidationFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidStorageConfigurations,
    StorageConfigValidatorFailureTest,
    ::testing::Values(

        FixtureCase{"DatabasePathEmpty", "database_path_empty.yaml"},

        FixtureCase{"BusyTimeoutNegative", "busy_timeout_negative.yaml"},

        FixtureCase{"BusyTimeoutOverLimit", "busy_timeout_over_limit.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第四组：合法边界测试
// ==================================================


// 最小超时时间：0ms
TEST_F(StorageConfigTest, ValidateMinimumBusyTimeoutBoundary)
{
    config::StorageConfig storage_config;

    ASSERT_TRUE(
        validateFixture("valid_zero_timeout.yaml", storage_config)
    ) << error_message;

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        0
    );
}


// 最大超时时间：60000ms
TEST_F(StorageConfigTest, ValidateMaximumBusyTimeoutBoundary)
{
    config::StorageConfig storage_config;

    ASSERT_TRUE(
        validateFixture("valid_max_timeout.yaml", storage_config)
    ) << error_message;

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        60000
    );
}


// ==================================================
// 第五组：直接测试 C++ 配置对象
// ==================================================


// Parser 解析失败后，原配置不应该被修改
TEST_F(StorageConfigTest, ParseFailureDoesNotModifyOriginalConfig)
{
    config::StorageConfig storage_config;

    storage_config.database_path = "original.db";
    storage_config.busy_timeout_ms = 3000;

    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("unknown_storage_key.yaml", root)
    ) << error_message;

    error_message.clear();

    EXPECT_FALSE(
        parser.parse(
            root,
            storage_config,
            error_message
        )
    );

    EXPECT_EQ(
        storage_config.database_path,
        "original.db"
    );

    EXPECT_EQ(
        storage_config.busy_timeout_ms,
        3000
    );
}


// 直接校验空数据库路径
TEST_F(StorageConfigTest, RejectEmptyDatabasePath)
{
    config::StorageConfig storage_config;

    storage_config.database_path.clear();

    EXPECT_FALSE(
        validator.validateStorageConfig(
            storage_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// 直接校验负数超时时间
TEST_F(StorageConfigTest, RejectNegativeBusyTimeout)
{
    config::StorageConfig storage_config;

    storage_config.busy_timeout_ms = -1;

    EXPECT_FALSE(
        validator.validateStorageConfig(
            storage_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}


// 直接校验超过上限的超时时间
TEST_F(StorageConfigTest, RejectBusyTimeoutOverLimit)
{
    config::StorageConfig storage_config;

    storage_config.busy_timeout_ms = 60001;

    EXPECT_FALSE(
        validator.validateStorageConfig(
            storage_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());
}
