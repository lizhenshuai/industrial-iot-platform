
# ==================================================
# 1. CMake 最低版本
# ==================================================

cmake_minimum_required(VERSION 3.16)


# ==================================================
# 2. 项目定义
# ==================================================

project(
    IndustrialGateway
    VERSION 1.0.0
    LANGUAGES CXX
)


# ==================================================
# 3. C++ 标准
# ==================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ==================================================
# 4. 测试配置
# ==================================================

include(CTest)


# ==================================================
# 5. 加载项目 CMake 配置
# ==================================================

include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildOptions.cmake"
)

include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompilerOptions.cmake"
)

include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake"
)

include(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake"
)


# ==================================================
# 6. 配置模块
# ==================================================

add_library(
    gateway_config
    STATIC

    src/infrastructure/config/YamlConfigLoader.cpp
    src/infrastructure/config/LoggerConfigParser.cpp
    src/infrastructure/config/MqttConfigParser.cpp
    src/infrastructure/config/StorageConfigParser.cpp
    src/infrastructure/config/GatewayConfigParser.cpp
    src/infrastructure/config/ConfigValidator.cpp
    src/infrastructure/config/CollectionConfigParser.cpp
)

target_include_directories(
    gateway_config
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    gateway_config
    PUBLIC

    yaml-cpp
)
target_link_libraries(gateway_config PRIVATE nlohmann_json::nlohmann_json)


# ==================================================
# 7. 日志实现模块
# ==================================================

add_library(
    gateway_logging
    STATIC

    src/infrastructure/logging/SpdlogLogger.cpp
)

target_include_directories(
    gateway_logging
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    gateway_logging
    PUBLIC

    spdlog::spdlog
)


# ==================================================
# 8. 并发基础设施模块
# ==================================================

add_library(
    gateway_concurrency
    STATIC

    src/infrastructure/concurrency/BoundedDataQueue.cpp
    src/infrastructure/concurrency/ConfigCommandQueue.cpp
    src/infrastructure/concurrency/ConfigAckQueue.cpp
)

target_include_directories(
    gateway_concurrency
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    gateway_concurrency
    PUBLIC

    Threads::Threads
)


# ==================================================
# 9. SQLite 存储基础设施模块
# ==================================================

add_library(
    gateway_storage
    STATIC

    src/infrastructure/storage/sqlite/SQLiteDatabase.cpp
    src/infrastructure/storage/sqlite/SQLiteMessageStorage.cpp
    src/infrastructure/storage/sqlite/SQLiteConfigStorage.cpp
)

target_include_directories(
    gateway_storage
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"

    PRIVATE

    "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/storage/sqlite"
)

target_link_libraries(
    gateway_storage
    PRIVATE

    SQLite::SQLite3
)


# ==================================================
# 10. 应用层数据接入模块
# ==================================================

add_library(
    gateway_ingestion
    STATIC

    src/application/ingestion/StorageWorker.cpp
)

target_include_directories(
    gateway_ingestion
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    gateway_ingestion
    PUBLIC

    Threads::Threads
    PRIVATE
    nlohmann_json::nlohmann_json
)


# 配置结果序列化，供唯一 MQTT 发布线程使用。
add_library(gateway_messaging STATIC
    src/application/messaging/ConfigAckSerializer.cpp
)
target_include_directories(gateway_messaging PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_messaging PRIVATE nlohmann_json::nlohmann_json)

# 应用层遥测消息及配置 ACK 发布 Worker
add_library(gateway_publishing STATIC
    src/application/publishing/MqttPublisherWorker.cpp
)
target_include_directories(gateway_publishing PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(gateway_publishing PUBLIC Threads::Threads gateway_concurrency gateway_messaging)

# 采集运行组件：每个协议端点由一个 Worker 串行调度。
add_library(gateway_collection STATIC
    src/application/collection/PollScheduler.cpp
    src/application/collection/TelemetryPacketBuilder.cpp
)
target_include_directories(gateway_collection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(gateway_connectors STATIC
    src/application/connector/ConnectorFactoryRegistry.cpp
    src/application/connector/ConnectorManager.cpp
    src/application/connector/ConnectionWorker.cpp
)
target_include_directories(gateway_connectors PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_connectors PUBLIC gateway_collection Threads::Threads)

add_library(gateway_modbus_tcp STATIC
    src/drivers/modbus_tcp/ModbusTcpConfigParser.cpp
    src/drivers/modbus_tcp/ModbusTcpConnector.cpp
    src/drivers/modbus_tcp/RegisterModbusTcpConnector.cpp
)
target_include_directories(gateway_modbus_tcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_modbus_tcp PUBLIC gateway_connectors
    PRIVATE yaml-cpp nlohmann_json::nlohmann_json PkgConfig::MODBUS)

# 本地串口 RTU 驱动，与 TCP 一样通过工厂登记，复用已安装的 libmodbus。
add_library(gateway_modbus_rtu STATIC
    src/drivers/modbus_rtu/ModbusRtuConfigParser.cpp
    src/drivers/modbus_rtu/ModbusRtuConnector.cpp
    src/drivers/modbus_rtu/RegisterModbusRtuConnector.cpp
)
target_include_directories(gateway_modbus_rtu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_modbus_rtu PUBLIC gateway_connectors
    PRIVATE yaml-cpp nlohmann_json::nlohmann_json PkgConfig::MODBUS)

# OPC UA 同步采集驱动，使用系统已有的客户端库。
add_library(gateway_opcua STATIC
    src/drivers/opcua/OpcUaConfigParser.cpp
    src/drivers/opcua/OpcUaConnector.cpp
    src/drivers/opcua/RegisterOpcUaConnector.cpp
)
target_include_directories(gateway_opcua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_opcua PUBLIC gateway_connectors
    PRIVATE yaml-cpp nlohmann_json::nlohmann_json open62541::open62541)

add_library(gateway_bootstrap STATIC
    src/bootstrap/RegisterBuiltInConnectors.cpp
    src/bootstrap/GatewayRuntime.cpp
)
target_include_directories(gateway_bootstrap PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_bootstrap PUBLIC gateway_modbus_tcp gateway_modbus_rtu gateway_opcua)

# 配置控制链路。
add_library(gateway_control STATIC
    src/application/agent/GatewayAgent.cpp
    src/application/config/ConfigManager.cpp
    src/application/config/ConnectorConfigApplier.cpp
)
target_include_directories(gateway_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(gateway_control PUBLIC gateway_connectors gateway_config gateway_concurrency
    Threads::Threads PRIVATE nlohmann_json::nlohmann_json)


# ==================================================
# 11. MQTT 基础设施模块
# ==================================================
#
# 负责：
#
# 1. Mosquitto 客户端生命周期管理。
# 2. MQTT Broker 连接与自动重连。
# 3. QoS 1 消息发布。
# 4. PUBACK 确认管理。
# 5. MQTT Topic 订阅。
# 6. SUBACK 确认管理。
# 7. MQTT 消息接收。
#
# ==================================================

add_library(
    gateway_mqtt
    STATIC

    src/infrastructure/mqtt/MosquittoMqttClient.cpp
)

target_include_directories(
    gateway_mqtt
    PUBLIC

    "${CMAKE_CURRENT_SOURCE_DIR}/include"

    PRIVATE

    "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/mqtt"
)

target_link_libraries(
    gateway_mqtt
    PRIVATE

    PkgConfig::MOSQUITTO
    Threads::Threads
)


# ==================================================
# 12. 网关程序与启动装配
# ==================================================

target_link_libraries(gateway_bootstrap PRIVATE
    gateway_control gateway_concurrency gateway_storage gateway_ingestion
    gateway_publishing gateway_mqtt
)

# 只使用 src/main.cpp 作为程序入口，根目录 main.cpp 不参与构建。
add_executable(industrial_gateway src/main.cpp)
target_link_libraries(industrial_gateway PRIVATE
    gateway_bootstrap gateway_config gateway_logging Threads::Threads
)


# ==================================================
# 13. 测试模块
# ==================================================

if(BUILD_TESTING)

    include(GoogleTest)


    # ==================================================
    # 12.1 日志配置单元测试
    # ==================================================

    add_executable(
        logger_config_tests

        tests/unit/infrastructure/config/LoggerConfigTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        logger_config_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/config"
    )


    # --------------------------------------------------
    # 配置测试数据目录
    # --------------------------------------------------

    target_compile_definitions(
        logger_config_tests
        PRIVATE

        "LOGGER_CONFIG_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/infrastructure/config/fixtures/logger\""
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        logger_config_tests
        PRIVATE

        gateway_config
        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        logger_config_tests
    )


    # ==================================================
    # 12.2 SQLite 存储配置单元测试
    # ==================================================

    add_executable(
        storage_config_tests

        tests/unit/infrastructure/config/StorageConfigTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        storage_config_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/config"
    )


    # --------------------------------------------------
    # 配置测试数据目录
    # --------------------------------------------------

    target_compile_definitions(
        storage_config_tests
        PRIVATE

        "STORAGE_CONFIG_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/infrastructure/config/fixtures/storage\""
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        storage_config_tests
        PRIVATE

        gateway_config
        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        storage_config_tests
    )


    # ==================================================
    # 12.3 MQTT 配置单元测试
    # ==================================================

    add_executable(
        mqtt_config_tests

        tests/unit/infrastructure/config/MqttConfigTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        mqtt_config_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/config"
    )


    # --------------------------------------------------
    # 配置测试数据目录
    # --------------------------------------------------

    target_compile_definitions(
        mqtt_config_tests
        PRIVATE

        "MQTT_CONFIG_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/infrastructure/config/fixtures/mqtt\""
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        mqtt_config_tests
        PRIVATE

        gateway_config
        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        mqtt_config_tests
    )


    # ==================================================
    # 12.4 Gateway 整体配置单元测试
    # ==================================================

    add_executable(
        gateway_config_tests

        tests/unit/infrastructure/config/GatewayConfigTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        gateway_config_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/config"
    )


    # --------------------------------------------------
    # 配置测试数据目录
    # --------------------------------------------------

    target_compile_definitions(
        gateway_config_tests
        PRIVATE

        "GATEWAY_CONFIG_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/infrastructure/config/fixtures/gateway\""
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        gateway_config_tests
        PRIVATE

        gateway_config
        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        gateway_config_tests
    )


    # ==================================================
    # 12.5 日志模块测试
    # ==================================================

    add_executable(
        spdlog_logger_tests

        tests/unit/infrastructure/logging/SpdlogLoggerTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        spdlog_logger_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/logging"

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/config"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        spdlog_logger_tests
        PRIVATE

        gateway_logging
        gateway_config

        GTest::gtest_main
        Threads::Threads
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        spdlog_logger_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )


    # ==================================================
    # 12.6 DataQueue 单元测试
    # ==================================================

    add_executable(
        bounded_data_queue_tests

        tests/unit/infrastructure/concurrency/BoundedDataQueueTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        bounded_data_queue_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/concurrency"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        bounded_data_queue_tests
        PRIVATE

        gateway_concurrency

        GTest::gtest_main
        Threads::Threads
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        bounded_data_queue_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )


    # ==================================================
    # 12.7 SQLite 数据库功能与性能测试
    # ==================================================

    add_executable(
        sqlite_database_tests

        tests/unit/infrastructure/storage/sqlite/SQLiteDatabaseTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        sqlite_database_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/storage/sqlite"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        sqlite_database_tests
        PRIVATE

        gateway_storage

        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        sqlite_database_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )


    # ==================================================
    # 12.8 SQLite 消息存储单元测试
    # ==================================================

    add_executable(
        sqlite_message_storage_tests

        tests/unit/infrastructure/storage/sqlite/SQLiteMessageStorageTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        sqlite_message_storage_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/storage/sqlite"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        sqlite_message_storage_tests
        PRIVATE

        gateway_storage

        GTest::gtest_main
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        sqlite_message_storage_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )


    # ==================================================
    # 12.9 StorageWorker 单元测试
    # ==================================================

    add_executable(
        storage_worker_tests

        tests/unit/application/ingestion/StorageWorkerTest.cpp
    )


    # --------------------------------------------------
    # 配置头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        storage_worker_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/application/ingestion"

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/concurrency"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        storage_worker_tests
        PRIVATE

        gateway_ingestion
        gateway_concurrency

        GTest::gtest_main
        Threads::Threads
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        storage_worker_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )

    # ==================================================
    # 12.10 Mosquitto MQTT 客户端测试
    # ==================================================

    add_executable(
        mosquitto_mqtt_client_tests

        tests/unit/infrastructure/mqtt/MosquittoMqttClientTest.cpp
    )


    # --------------------------------------------------
    # MQTT 头文件搜索路径
    # --------------------------------------------------

    target_include_directories(
        mosquitto_mqtt_client_tests
        PRIVATE

        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/mqtt"
    )


    # --------------------------------------------------
    # 链接依赖
    # --------------------------------------------------

    target_link_libraries(
        mosquitto_mqtt_client_tests
        PRIVATE

        gateway_mqtt
        yaml-cpp

        GTest::gtest_main
        Threads::Threads
    )


    # --------------------------------------------------
    # 自动发现 GoogleTest 测试用例
    # --------------------------------------------------

    gtest_discover_tests(
        mosquitto_mqtt_client_tests

        PROPERTIES

        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        TIMEOUT 30
    )

    add_executable(mqtt_publisher_worker_tests
        tests/unit/application/publishing/MqttPublisherWorkerTest.cpp
    )
    target_include_directories(mqtt_publisher_worker_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/application/publishing"
    )
    target_link_libraries(mqtt_publisher_worker_tests PRIVATE
        gateway_publishing GTest::gtest_main Threads::Threads
    )
    gtest_discover_tests(mqtt_publisher_worker_tests
        PROPERTIES TIMEOUT 15
    )

    # 真实队列 / SQLite / MQTT Broker 链路测试，需显式运行 DISABLED_ 用例。
    add_executable(telemetry_pipeline_tests
        tests/integration/publishing/TelemetryPipelineTest.cpp
    )
    target_include_directories(telemetry_pipeline_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/application/ingestion"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/application/publishing"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/application/messaging"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/concurrency"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/storage/sqlite"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/infrastructure/mqtt"
    )
    target_link_libraries(telemetry_pipeline_tests PRIVATE
        gateway_ingestion gateway_publishing gateway_concurrency gateway_storage
        gateway_mqtt yaml-cpp GTest::gtest_main Threads::Threads
    )
    gtest_discover_tests(telemetry_pipeline_tests PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        LABELS integration
        TIMEOUT 90
    )

endif()
