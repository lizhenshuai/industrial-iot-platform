
# ==================================================
# Third-party Dependencies
# ==================================================


# ==================================================
# 1. Production Dependencies
# ==================================================

# --------------------------------------------------
# spdlog
# --------------------------------------------------

find_package(
    spdlog
    CONFIG
    REQUIRED
)


# --------------------------------------------------
# yaml-cpp
# --------------------------------------------------

find_package(
    yaml-cpp
    CONFIG
    REQUIRED
)


# --------------------------------------------------
# SQLite
# --------------------------------------------------

find_package(
    SQLite3
    REQUIRED
)

# 使用用户已安装的 JSON 头文件库，不下载依赖。
find_package(nlohmann_json 3.10.5 CONFIG REQUIRED)


# --------------------------------------------------
# 线程库
# --------------------------------------------------

find_package(
    Threads
    REQUIRED
)


# --------------------------------------------------
# Mosquitto MQTT
# --------------------------------------------------
#
# 使用 pkg-config 查找系统安装的 libmosquitto。
#
# 导入目标：
# PkgConfig::MOSQUITTO
#
# 后续 MQTT 模块通过 target_link_libraries
# 链接该目标。
#
# --------------------------------------------------

find_package(
    PkgConfig
    REQUIRED
)

pkg_check_modules(
    MOSQUITTO
    REQUIRED
    IMPORTED_TARGET
    libmosquitto
)

pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus)

# 复用用户已安装的 open62541；不构建目录内第三方源码，也不下载依赖。
find_package(open62541 1.5 CONFIG REQUIRED)


# ==================================================
# 2. Test Dependencies
# ==================================================

if(BUILD_TESTING)

    # GoogleTest
    find_package(
        GTest
        REQUIRED
    )

endif()
