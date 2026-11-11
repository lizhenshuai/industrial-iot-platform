
#include "YamlConfigLoader.hpp"
#include "MqttConfigParser.hpp"
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

class MqttConfigTest : public ::testing::Test
{
protected:

    // YAML 测试文件目录
    fs::path fixtureDirectory() const
    {
        return fs::path(MQTT_CONFIG_FIXTURE_DIR);
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
        config::MqttConfig& mqtt_config
    )
    {
        YAML::Node root;

        if (!loadFixture(filename, root))
        {
            return false;
        }

        return parser.parse(
            root,
            mqtt_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 读取、解析并校验 YAML 文件
    // --------------------------------------------------

    bool validateFixture(
        const std::string& filename,
        config::MqttConfig& mqtt_config
    )
    {
        if (!parseFixture(filename, mqtt_config))
        {
            return false;
        }

        return validator.validateMqttConfig(
            mqtt_config,
            error_message
        );
    }


    // --------------------------------------------------
    // 断言：配置解析失败
    // --------------------------------------------------

    void expectParseFailure(const std::string& filename)
    {
        config::MqttConfig mqtt_config;

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
                mqtt_config,
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
        config::MqttConfig mqtt_config;

        // 首先必须解析成功
        ASSERT_TRUE(
            parseFixture(filename, mqtt_config)
        ) << "配置解析失败: " << error_message;

        error_message.clear();

        // Validator 必须失败
        EXPECT_FALSE(
            validator.validateMqttConfig(
                mqtt_config,
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
        config::MqttConfig mqtt_config;

        ASSERT_TRUE(
            validateFixture(filename, mqtt_config)
        ) << "配置验证失败: " << error_message;
    }


protected:

    // YAML 加载器
    infrastructure_config::YamlConfigLoader loader;

    // MQTT 配置解析器
    infrastructure_config::MqttConfigParser parser;

    // 配置校验器
    infrastructure_config::ConfigValidator validator;

    // 错误信息
    std::string error_message;
};


// ==================================================
// 第一组：YamlConfigLoader 测试
// ==================================================


// 正常读取 YAML 文件
TEST_F(MqttConfigTest, LoadValidFile)
{
    YAML::Node root;

    ASSERT_TRUE(loadFixture("valid_full.yaml", root))
        << error_message;

    EXPECT_TRUE(root.IsMap());

    EXPECT_TRUE(root["mqtt"].IsMap());
}


// 文件不存在
TEST_F(MqttConfigTest, LoadNonexistentFile)
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


// 空文件路径
TEST_F(MqttConfigTest, LoadEmptyPath)
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
// 第二组：正常 MQTT 配置测试
// ==================================================


// 完整 MQTT 配置
TEST_F(MqttConfigTest, ParseFullConfig)
{
    config::MqttConfig mqtt_config;

    ASSERT_TRUE(
        validateFixture("valid_full.yaml", mqtt_config)
    ) << error_message;

    EXPECT_TRUE(mqtt_config.enabled);

    EXPECT_EQ(
        mqtt_config.host,
        "192.168.1.100"
    );

    EXPECT_EQ(
        mqtt_config.port,
        1883
    );

    EXPECT_EQ(
        mqtt_config.client_id,
        "gateway-test"
    );

    EXPECT_EQ(
        mqtt_config.keep_alive,
        120
    );
}


// 最小 MQTT 配置及默认值
TEST_F(MqttConfigTest, ParseMinimalConfig)
{
    config::MqttConfig mqtt_config;

    ASSERT_TRUE(
        validateFixture("valid_minimal.yaml", mqtt_config)
    ) << error_message;

    const config::MqttConfig defaults;

    EXPECT_EQ(
        mqtt_config.enabled,
        defaults.enabled
    );

    EXPECT_EQ(
        mqtt_config.host,
        defaults.host
    );

    EXPECT_EQ(
        mqtt_config.port,
        defaults.port
    );

    EXPECT_EQ(
        mqtt_config.client_id,
        defaults.client_id
    );

    EXPECT_EQ(
        mqtt_config.keep_alive,
        defaults.keep_alive
    );
}


// mqtt 配置段缺失
TEST_F(MqttConfigTest, ParseMissingMqtt)
{
    config::MqttConfig mqtt_config;

    ASSERT_TRUE(
        validateFixture("missing_mqtt.yaml", mqtt_config)
    ) << error_message;

    const config::MqttConfig defaults;

    EXPECT_EQ(mqtt_config.enabled, defaults.enabled);
    EXPECT_EQ(mqtt_config.host, defaults.host);
    EXPECT_EQ(mqtt_config.port, defaults.port);
    EXPECT_EQ(mqtt_config.client_id, defaults.client_id);
    EXPECT_EQ(mqtt_config.keep_alive, defaults.keep_alive);
}


// MQTT 禁用时，即使连接参数不合法也允许通过
TEST_F(MqttConfigTest, DisabledMqttSkipsConnectionValidation)
{
    config::MqttConfig mqtt_config;

    ASSERT_TRUE(
        validateFixture(
            "disabled_invalid_parameters.yaml",
            mqtt_config
        )
    ) << error_message;

    EXPECT_FALSE(mqtt_config.enabled);

    EXPECT_TRUE(mqtt_config.host.empty());

    EXPECT_EQ(mqtt_config.port, 0);

    EXPECT_TRUE(mqtt_config.client_id.empty());

    EXPECT_EQ(mqtt_config.keep_alive, -1);
}


// ==================================================
// 第三组：Parser 异常测试
// ==================================================

struct FixtureCase
{
    const char* test_name;
    const char* filename;
};


class MqttConfigParserFailureTest
    : public MqttConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    MqttConfigParserFailureTest,
    RejectInvalidConfiguration
)
{
    expectParseFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidParserConfigurations,
    MqttConfigParserFailureTest,
    ::testing::Values(

        FixtureCase{"RootNotMap", "root_not_map.yaml"},

        FixtureCase{"MqttNotMap", "mqtt_not_map.yaml"},

        FixtureCase{"UnknownMqttKey", "unknown_mqtt_key.yaml"},

        FixtureCase{"EnabledWrongType", "enabled_wrong_type.yaml"},

        FixtureCase{"HostWrongType", "host_wrong_type.yaml"},

        FixtureCase{"PortWrongType", "port_wrong_type.yaml"},

        FixtureCase{"ClientIdWrongType", "client_id_wrong_type.yaml"},

        FixtureCase{"KeepAliveWrongType", "keep_alive_wrong_type.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第四组：Validator 异常测试
// ==================================================

class MqttConfigValidatorFailureTest
    : public MqttConfigTest,
      public ::testing::WithParamInterface<FixtureCase>
{
};


TEST_P(
    MqttConfigValidatorFailureTest,
    RejectInvalidConfiguration
)
{
    expectValidationFailure(GetParam().filename);
}


INSTANTIATE_TEST_SUITE_P(
    InvalidMqttConfigurations,
    MqttConfigValidatorFailureTest,
    ::testing::Values(

        FixtureCase{"EmptyHost", "empty_host.yaml"},

        FixtureCase{"EmptyClientId", "empty_client_id.yaml"},

        FixtureCase{"PortZero", "port_zero.yaml"},

        FixtureCase{"PortOverLimit", "port_over_limit.yaml"},

        FixtureCase{"KeepAliveNegative", "keep_alive_negative.yaml"},

        FixtureCase{"KeepAliveOverLimit", "keep_alive_over_limit.yaml"}

    ),
    [](const ::testing::TestParamInfo<FixtureCase>& info)
    {
        return info.param.test_name;
    }
);


// ==================================================
// 第五组：合法边界测试
// ==================================================


// MQTT 端口合法边界
TEST_F(MqttConfigTest, ValidatePortBoundaries)
{
    config::MqttConfig mqtt_config;

    mqtt_config.port = 1;

    EXPECT_TRUE(
        validator.validateMqttConfig(
            mqtt_config,
            error_message
        )
    ) << error_message;

    mqtt_config.port = 65535;

    EXPECT_TRUE(
        validator.validateMqttConfig(
            mqtt_config,
            error_message
        )
    ) << error_message;
}


// MQTT Keep Alive 合法边界
TEST_F(MqttConfigTest, ValidateKeepAliveBoundaries)
{
    config::MqttConfig mqtt_config;

    mqtt_config.keep_alive = 0;

    EXPECT_TRUE(
        validator.validateMqttConfig(
            mqtt_config,
            error_message
        )
    ) << error_message;

    mqtt_config.keep_alive = 65535;

    EXPECT_TRUE(
        validator.validateMqttConfig(
            mqtt_config,
            error_message
        )
    ) << error_message;
}


// ==================================================
// 第六组：配置异常保护测试
// ==================================================


// 解析失败不能修改原配置
TEST_F(MqttConfigTest, ParseFailureDoesNotModifyOriginalConfig)
{
    config::MqttConfig mqtt_config;

    mqtt_config.enabled = false;
    mqtt_config.host = "original-host";
    mqtt_config.port = 4321;
    mqtt_config.client_id = "original-client";
    mqtt_config.keep_alive = 77;

    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("port_wrong_type.yaml", root)
    ) << error_message;

    error_message.clear();

    EXPECT_FALSE(
        parser.parse(
            root,
            mqtt_config,
            error_message
        )
    );

    EXPECT_FALSE(error_message.empty());

    // 原配置必须保持不变
    EXPECT_FALSE(mqtt_config.enabled);

    EXPECT_EQ(mqtt_config.host, "original-host");

    EXPECT_EQ(mqtt_config.port, 4321);

    EXPECT_EQ(mqtt_config.client_id, "original-client");

    EXPECT_EQ(mqtt_config.keep_alive, 77);
}


// 解析成功清理之前的错误信息
TEST_F(MqttConfigTest, SuccessfulParseClearsPreviousError)
{
    YAML::Node root;

    ASSERT_TRUE(
        loadFixture("valid_full.yaml", root)
    ) << error_message;

    error_message = "previous error";

    config::MqttConfig mqtt_config;

    ASSERT_TRUE(
        parser.parse(
            root,
            mqtt_config,
            error_message
        )
    ) << error_message;

    EXPECT_TRUE(error_message.empty());
}


// 校验成功清理之前的错误信息
TEST_F(MqttConfigTest, SuccessfulValidationClearsPreviousError)
{
    config::MqttConfig mqtt_config;

    error_message = "previous error";

    EXPECT_TRUE(
        validator.validateMqttConfig(
            mqtt_config,
            error_message
        )
    ) << error_message;

    EXPECT_TRUE(error_message.empty());
}
