cmake_minimum_required(VERSION 3.16)
project(GatewayOpcUaE2ESimulator LANGUAGES CXX)

# 独立构建测试辅助进程；不加入网关生产目标、不下载依赖、不运行测试。
find_package(open62541 1.5 CONFIG REQUIRED)
find_package(nlohmann_json 3.10.5 CONFIG REQUIRED)
add_executable(opcua_e2e_server OpcUaTestServer.cpp)
target_compile_features(opcua_e2e_server PRIVATE cxx_std_17)
target_link_libraries(opcua_e2e_server PRIVATE open62541::open62541 nlohmann_json::nlohmann_json)
