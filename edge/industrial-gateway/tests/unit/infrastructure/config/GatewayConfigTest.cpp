
#include "YamlConfigLoader.hpp"
#include "GatewayConfigParser.hpp"
#include "ConfigValidator.hpp"

#include "industrial_gateway/common/LogLevel.hpp"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <string>


namespace fs = std::filesystem;

namespace config = industrial_gateway::config;

namespace infrastructure_config =
    industrial_gateway::infrastructure::config;


// ==================================================
// 测试参数
// ==================================================

struct GatewayFixtureCase
{
    const char* test_name;
    const char* filename;
};


// ==================================================
// 网关整体配置测试
// ==================================================

class GatewayConfigTest : public ::testing::Test
{
protected:

    // ==================================================
    // 测试文件路径
    // ==================================================

    fs::path fixtureDirectory() const
    {
        return fs::path(GATEWAY_CONFIG_FIXTURE_DIR);
    }


    fs::path fixturePath(const std::string& filename) const
    {
        return fixtureDirectory() / filename;
    }


    // ==================================================
    // YAML 文件加载
    // ==================================================

    bool loadFixture(
        const std::string& filename,
        YAML::Node& root
    )
    {
        error_message_.clear();

        return loader_.loadFile(
            fixturePath(filename),
            root,
            error_message_
        );
    }


    // ==================================================
    // YAML 加载 + 网关配置解析
    // ==================================================

    bool parseFixture(
        const std::string& filename,
        config::GatewayConfig& gateway_config
    )
    {
        YAML::Node root;

        if (!loadFixture(filename, root))
        {
            return false;
        }

        return parser_.parse(
            root,
            gateway_config,
            error_message_
        );
    }


    // ==================================================
    // YAML 加载 + 配置解析 + 整体校验
    // ==================================================

    bool validateFixture(
        const std::string& filename,
        config::GatewayConfig& gateway_config
    )
    {
        if (!parseFixture(filename, gateway_config))
        {
            return false;
        }

        return validator_.validateGatewayConfig(
            gateway_config,
            error_message_
        );
    }


    // ==================================================
    // 断言：配置解析失败
    // ==================================================

    void expectParseFailure(const std::string& filename)
    {
        YAML::Node root;

        ASSERT_TRUE(loadFixture(filename, root))
            << "YAML 文件读取失败: "
            << error_message_;

        config::GatewayConfig gateway_config;

        error_message_.clear();

        EXPECT_FALSE(
            parser_.parse(
                root,
                gateway_config,
                error_message_
            )
        ) << "预期解析失败，但实际成功: " << filename;

        EXPECT_FALSE(error_message_.empty())
            << "解析失败时没有返回错误信息: " << filename;
    }


    // ==================================================
    // 断言：整体配置校验失败
    // ==================================================

    void expectValidationFailure(const std::string& filename)
    {
        config::GatewayConfig gateway_config;

        // 配置解析必须成功
        ASSERT_TRUE(
            parseFixture(filename, gateway_config)
        ) << "配置解析失败: " << error_message_;

        error_message_.clear();

        // 整体配置校验必须失败
        EXPECT_FALSE(
            validator_.validateGatewayConfig(
                gateway_config,
                error_message_
            )
        ) << "预期校验失败，但实际成功: " << filename;

        EXPECT_FALSE(error_message_.empty())
            << "校验失败时没有返回错误信息: " << filename;
    }


protected:

    infrastructure_config::YamlConfigLoader loader_;

    infrastructure_config::GatewayConfigParser parser_;

    infrastructure_config::ConfigValidator validator_;

    std::string error_message_;
};


// ==================================================
// 第一组：YamlConfigLoader 测试
// ==================================================


// 正常读取完整配置文件
TEST_F(
    GatewayConfigTest,
    LoadFullConfigurationFile
)
{
    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("valid_full.yaml", root)
    ) << error_message_;

    EXPECT_TRUE(root.IsMap());

    EXPECT_TRUE(root["schema_version"].IsDefined());

    EXPECT_TRUE(root["gateway"].IsMap());

    EXPECT_TRUE(root["logging"].IsMap());

    EXPECT_TRUE(root["mqtt"].IsMap());

    EXPECT_TRUE(root["storage"].IsMap());
}


// 配置文件不存在
TEST_F(
    GatewayConfigTest,
    LoadNonexistentFile
)
{
    YAML::Node root;

    EXPECT_FALSE(
        loader_.loadFile(
            fixturePath("__nonexistent__.yaml"),
            root,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());
}


// YAML 语法错误
TEST_F(
    GatewayConfigTest,
    RejectInvalidYamlSyntax
)
{
    YAML::Node root;

    EXPECT_FALSE(
        loadFixture("invalid_syntax.yaml", root)
    );

    EXPECT_FALSE(error_message_.empty());
}


// ==================================================
// 第二组：完整配置解析与校验
// ==================================================


// 完整配置读取成功
TEST_F(
    GatewayConfigTest,
    ParseAndValidateFullConfiguration
)
{
    config::GatewayConfig gateway_config;

    ASSERT_TRUE(
        validateFixture("valid_full.yaml", gateway_config)
    ) << error_message_;


    // --------------------------------------------------
    // 网关基础配置
    // --------------------------------------------------

    EXPECT_EQ(gateway_config.schema_version, 1);

    EXPECT_EQ(
        gateway_config.gateway_id,
        "industrial-gateway-test"
    );


    // --------------------------------------------------
    // 日志配置
    // --------------------------------------------------

    EXPECT_EQ(
        gateway_config.logging.level,
        industrial_gateway::common::LogLevel::Info
    );

    EXPECT_TRUE(gateway_config.logging.console_enabled);

    EXPECT_TRUE(gateway_config.logging.file_enabled);

    EXPECT_EQ(
        gateway_config.logging.file_path,
        "logs/gateway-test.log"
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.strategy,
        config::LogRotationStrategy::Size
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.max_size_bytes,
        10485760
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.max_files,
        5
    );


    // --------------------------------------------------
    // MQTT 配置
    // --------------------------------------------------

    EXPECT_TRUE(gateway_config.mqtt.enabled);

    EXPECT_EQ(
        gateway_config.mqtt.host,
        "127.0.0.1"
    );

    EXPECT_EQ(gateway_config.mqtt.port, 1883);

    EXPECT_EQ(
        gateway_config.mqtt.client_id,
        "industrial-gateway-test"
    );

    EXPECT_EQ(gateway_config.mqtt.keep_alive, 60);


    // --------------------------------------------------
    // SQLite 配置
    // --------------------------------------------------

    EXPECT_EQ(
        gateway_config.storage.database_path,
        "data/gateway-test.db"
    );

    EXPECT_EQ(
        gateway_config.storage.busy_timeout_ms,
        5000
    );
}


// 最小配置读取成功，其余配置使用默认值
TEST_F(
    GatewayConfigTest,
    ParseMinimalConfigurationWithDefaults
)
{
    config::GatewayConfig gateway_config;

    ASSERT_TRUE(
        validateFixture("valid_minimal.yaml", gateway_config)
    ) << error_message_;

    const config::GatewayConfig defaults;


    // --------------------------------------------------
    // 网关基础配置
    // --------------------------------------------------

    EXPECT_EQ(gateway_config.schema_version, 1);

    EXPECT_EQ(
        gateway_config.gateway_id,
        "industrial-gateway-minimal"
    );


    // --------------------------------------------------
    // 日志默认值
    // --------------------------------------------------

    EXPECT_EQ(
        gateway_config.logging.level,
        defaults.logging.level
    );

    EXPECT_EQ(
        gateway_config.logging.console_enabled,
        defaults.logging.console_enabled
    );

    EXPECT_EQ(
        gateway_config.logging.file_enabled,
        defaults.logging.file_enabled
    );

    EXPECT_EQ(
        gateway_config.logging.file_path,
        defaults.logging.file_path
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.strategy,
        defaults.logging.rotation.strategy
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.max_size_bytes,
        defaults.logging.rotation.max_size_bytes
    );

    EXPECT_EQ(
        gateway_config.logging.rotation.max_files,
        defaults.logging.rotation.max_files
    );


    // --------------------------------------------------
    // MQTT 默认值
    // --------------------------------------------------

    EXPECT_EQ(
        gateway_config.mqtt.enabled,
        defaults.mqtt.enabled
    );

    EXPECT_EQ(
        gateway_config.mqtt.host,
        defaults.mqtt.host
    );

    EXPECT_EQ(
        gateway_config.mqtt.port,
        defaults.mqtt.port
    );

    EXPECT_EQ(
        gateway_config.mqtt.client_id,
        defaults.mqtt.client_id
    );

    EXPECT_EQ(
        gateway_config.mqtt.keep_alive,
        defaults.mqtt.keep_alive
    );


    // --------------------------------------------------
    // SQLite 默认值
    // --------------------------------------------------

    EXPECT_EQ(
        gateway_config.storage.database_path,
        defaults.storage.database_path
    );

    EXPECT_EQ(
        gateway_config.storage.busy_timeout_ms,
        defaults.storage.busy_timeout_ms
    );
}


// ==================================================
// 第三组：GatewayConfigParser 异常测试
// ==================================================

class GatewayConfigParserFailureTest
    : public GatewayConfigTest,
      public ::testing::WithParamInterface<GatewayFixtureCase>
{
};


TEST_P(
    GatewayConfigParserFailureTest,
    RejectInvalidConfiguration
)
{
    expectParseFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidGatewayConfigurations,
    GatewayConfigParserFailureTest,
    ::testing::Values(

        GatewayFixtureCase{
            "RootNotMap",
            "root_not_map.yaml"
        },

        GatewayFixtureCase{
            "MissingSchemaVersion",
            "missing_schema_version.yaml"
        },

        GatewayFixtureCase{
            "SchemaVersionWrongType",
            "schema_version_wrong_type.yaml"
        },

        GatewayFixtureCase{
            "MissingGateway",
            "missing_gateway.yaml"
        },

        GatewayFixtureCase{
            "GatewayNotMap",
            "gateway_not_map.yaml"
        },

        GatewayFixtureCase{
            "MissingGatewayId",
            "missing_gateway_id.yaml"
        },

        GatewayFixtureCase{
            "GatewayIdWrongType",
            "gateway_id_wrong_type.yaml"
        },

        GatewayFixtureCase{
            "UnknownRootKey",
            "unknown_root_key.yaml"
        },

        GatewayFixtureCase{
            "UnknownGatewayKey",
            "unknown_gateway_key.yaml"
        }

    ),
    [](const ::testing::TestParamInfo<GatewayFixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第四组：ConfigValidator 整体校验异常测试
// ==================================================

class GatewayConfigValidatorFailureTest
    : public GatewayConfigTest,
      public ::testing::WithParamInterface<GatewayFixtureCase>
{
};


TEST_P(
    GatewayConfigValidatorFailureTest,
    RejectInvalidConfiguration
)
{
    expectValidationFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidGatewayConfigurations,
    GatewayConfigValidatorFailureTest,
    ::testing::Values(

        GatewayFixtureCase{
            "UnsupportedSchemaVersion",
            "unsupported_schema_version.yaml"
        },

        GatewayFixtureCase{
            "EmptyGatewayId",
            "gateway_id_empty.yaml"
        },

        GatewayFixtureCase{
            "InvalidMqttPort",
            "mqtt_invalid_port.yaml"
        },

        GatewayFixtureCase{
            "InvalidStorageTimeout",
            "storage_invalid_timeout.yaml"
        }

    ),
    [](const ::testing::TestParamInfo<GatewayFixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第五组：配置异常保护测试
// ==================================================


// 解析失败不能修改原有 GatewayConfig
TEST_F(
    GatewayConfigTest,
    ParseFailureDoesNotModifyOriginalConfiguration
)
{
    config::GatewayConfig gateway_config;

    // 设置原始配置
    gateway_config.schema_version = 99;

    gateway_config.gateway_id = "original-gateway";

    gateway_config.mqtt.enabled = false;

    gateway_config.mqtt.host = "original-host";

    gateway_config.mqtt.port = 4321;

    gateway_config.storage.database_path =
        "original.db";

    gateway_config.storage.busy_timeout_ms = 1234;


    // 使用错误配置文件
    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("schema_version_wrong_type.yaml", root)
    ) << error_message_;

    error_message_.clear();

    EXPECT_FALSE(
        parser_.parse(
            root,
            gateway_config,
            error_message_
        )
    );

    EXPECT_FALSE(error_message_.empty());


    // 原有配置必须保持不变
    EXPECT_EQ(gateway_config.schema_version, 99);

    EXPECT_EQ(
        gateway_config.gateway_id,
        "original-gateway"
    );

    EXPECT_FALSE(gateway_config.mqtt.enabled);

    EXPECT_EQ(
        gateway_config.mqtt.host,
        "original-host"
    );

    EXPECT_EQ(gateway_config.mqtt.port, 4321);

    EXPECT_EQ(
        gateway_config.storage.database_path,
        "original.db"
    );

    EXPECT_EQ(
        gateway_config.storage.busy_timeout_ms,
        1234
    );
}


// Parser 成功后清理之前的错误信息
TEST_F(
    GatewayConfigTest,
    SuccessfulParseClearsPreviousError
)
{
    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("valid_full.yaml", root)
    ) << error_message_;

    error_message_ = "previous error";

    config::GatewayConfig gateway_config;

    ASSERT_TRUE(
        parser_.parse(
            root,
            gateway_config,
            error_message_
        )
    ) << error_message_;

    EXPECT_TRUE(error_message_.empty());
}


// 整体校验成功后清理之前的错误信息
TEST_F(
    GatewayConfigTest,
    SuccessfulValidationClearsPreviousError
)
{
    config::GatewayConfig gateway_config;

    error_message_ = "previous error";

    EXPECT_TRUE(
        validator_.validateGatewayConfig(
            gateway_config,
            error_message_
        )
    ) << error_message_;

    EXPECT_TRUE(error_message_.empty());
}
