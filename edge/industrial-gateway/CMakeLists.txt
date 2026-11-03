cmake_minimum_required(VERSION 3.16)

project(ModbusCollector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(
    LIBMODBUS
    REQUIRED
    IMPORTED_TARGET
    libmodbus
)

add_executable(
    modbus_collector
    main.cpp
)

target_link_libraries(
    modbus_collector
    PRIVATE
    PkgConfig::LIBMODBUS
)