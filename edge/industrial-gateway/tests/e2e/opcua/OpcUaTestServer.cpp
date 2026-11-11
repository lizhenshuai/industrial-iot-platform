// 测试专用服务器，不调用生产驱动、生产解析器或生产解码函数。
// 数据源接口参考 https://open62541.org/doc/master/tutorial_server_datasource.html
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <nlohmann/json.hpp>

#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
using Json = nlohmann::json;
volatile std::sig_atomic_t stopping = 0;
void stopSignal(int) { stopping = 1; }

void good(UA_StatusCode status, const char* operation)
{
    if (!UA_StatusCode_isGood(status))
        throw std::runtime_error(std::string(operation) + ": " + UA_StatusCode_name(status));
}

template<class T, std::size_t Index>
struct UaOwned
{
    T value{};
    UaOwned() = default;
    ~UaOwned() { UA_clear(&value, &UA_TYPES[Index]); }
    UaOwned(const UaOwned&) = delete;
    UaOwned& operator=(const UaOwned&) = delete;
};

struct ConfigOwner
{
    UA_ServerConfig value{};
    ~ConfigOwner() { UA_ServerConfig_clear(&value); }
};

struct ServerOwner
{
    UA_Server* value{nullptr};
    bool started{false};
    ~ServerOwner()
    {
        if (!value) return;
        if (started) (void)UA_Server_run_shutdown(value);
        (void)UA_Server_delete(value);
    }
};

struct Observation
{
    std::ofstream output;
    bool recording{false};
    bool failed{false};
};

struct Node
{
    std::int64_t device_id{};
    std::string node_id;
    UaOwned<UA_Variant, UA_TYPES_VARIANT> stored;
    Observation* observation{};
};

template<class T>
T integer(const Json& value)
{
    if (!value.is_number_integer()) throw std::runtime_error("整数 fixture 类型不正确");
    if constexpr (std::is_signed_v<T>)
    {
        if (value.is_number_unsigned() && value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
            throw std::runtime_error("有符号 fixture 溢出");
        const auto number = value.get<std::int64_t>();
        if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max())
            throw std::runtime_error("有符号 fixture 溢出");
        return static_cast<T>(number);
    }
    else
    {
        if (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)
            throw std::runtime_error("无符号 fixture 不能为负数");
        const auto number = value.get<std::uint64_t>();
        if (number > std::numeric_limits<T>::max()) throw std::runtime_error("无符号 fixture 溢出");
        return static_cast<T>(number);
    }
}

template<class T>
void store(Node& node, T value, std::size_t index)
{
    good(UA_Variant_setScalarCopy(&node.stored.value, &value, &UA_TYPES[index]), "创建节点值");
}

void fill(Node& node, const Json& definition)
{
    const auto type = definition.at("type").get<std::string>();
    const auto& value = definition.at("value");
    if (type == "bool") store(node, UA_Boolean(value.get<bool>()), UA_TYPES_BOOLEAN);
    else if (type == "int16") store(node, integer<UA_Int16>(value), UA_TYPES_INT16);
    else if (type == "uint16") store(node, integer<UA_UInt16>(value), UA_TYPES_UINT16);
    else if (type == "int32") store(node, integer<UA_Int32>(value), UA_TYPES_INT32);
    else if (type == "uint32") store(node, integer<UA_UInt32>(value), UA_TYPES_UINT32);
    else if (type == "int64") store(node, integer<UA_Int64>(value), UA_TYPES_INT64);
    else if (type == "uint64") store(node, integer<UA_UInt64>(value), UA_TYPES_UINT64);
    else if (type == "float") store(node, value.get<UA_Float>(), UA_TYPES_FLOAT);
    else if (type == "double") store(node, value.get<UA_Double>(), UA_TYPES_DOUBLE);
    else if (type == "string")
    {
        auto text = value.get<std::string>();
        // 显式长度保留内嵌 NUL；setScalarCopy 会复制内容，不保留 text 的指针。
        UA_String source{text.size(), reinterpret_cast<UA_Byte*>(text.data())};
        store(node, source, UA_TYPES_STRING);
    }
    else throw std::runtime_error("不支持的 fixture 类型: " + type);
}

UA_StatusCode readNode(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void* context,
                       UA_Boolean timestamp, const UA_NumericRange* range, UA_DataValue* value)
{
    auto& node = *static_cast<Node*>(context);
    try
    {
        if (range) return UA_STATUSCODE_BADINDEXRANGEINVALID;
        const auto status = UA_Variant_copy(&node.stored.value, &value->value);
        if (!UA_StatusCode_isGood(status)) return status;
        value->hasValue = true;
        if (timestamp)
        {
            value->sourceTimestamp = UA_DateTime_now();
            value->hasSourceTimestamp = true;
        }
        auto& observation = *node.observation;
        if (observation.recording)
        {
            timespec mono{}, wall{};
            if (::clock_gettime(CLOCK_MONOTONIC, &mono) != 0 || ::clock_gettime(CLOCK_REALTIME, &wall) != 0)
                throw std::runtime_error("读取时钟失败");
            // 与 Linux Python time.monotonic 使用相同系统时钟；不能使用进程启动后的相对时间。
            Json row{{"protocol", "opcua"}, {"kind", "node_read"},
                     {"device_id", node.device_id}, {"node_id", node.node_id},
                     {"monotonic", static_cast<double>(mono.tv_sec) + mono.tv_nsec / 1e9},
                     {"wall_ms", static_cast<std::int64_t>(wall.tv_sec) * 1000 + wall.tv_nsec / 1000000}};
            observation.output << row.dump() << '\n';
            observation.output.flush();
            if (!observation.output) throw std::runtime_error("写入节点读取日志失败");
        }
        return UA_STATUSCODE_GOOD;
    }
    catch (...)
    {
        // C 回调边界不传播 C++ 异常；主循环随后失败退出，控制脚本会检测进程异常。
        node.observation->failed = true;
        return UA_STATUSCODE_BADINTERNALERROR;
    }
}

void addNode(UA_Server* server, Node& node)
{
    UaOwned<UA_NodeId, UA_TYPES_NODEID> id;
    good(UA_NodeId_parse(&id.value, UA_STRING(const_cast<char*>(node.node_id.c_str()))), "解析 fixture NodeId");
    if (id.value.namespaceIndex != 2) throw std::runtime_error("fixture 必须使用 namespace 2");
    UaOwned<UA_VariableAttributes, UA_TYPES_VARIABLEATTRIBUTES> attributes;
    attributes.value = UA_VariableAttributes_default;
    attributes.value.displayName = UA_LOCALIZEDTEXT_ALLOC("en-US", node.node_id.c_str());
    if (!attributes.value.displayName.text.data) throw std::bad_alloc();
    attributes.value.dataType = node.stored.value.type->typeId; // 内置类型 ID 为数字，无需动态复制。
    attributes.value.valueRank = UA_VALUERANK_SCALAR;
    attributes.value.accessLevel = UA_ACCESSLEVELMASK_READ;
    good(UA_Variant_copy(&node.stored.value, &attributes.value.value), "复制初始值");
    UA_DataSource source{};
    source.read = readNode;
    good(UA_Server_addDataSourceVariableNode(server, id.value,
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(2, const_cast<char*>(node.node_id.c_str())),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attributes.value, source, &node, nullptr),
        "添加测试节点");
}

int run(int argc, char** argv)
{
    std::map<std::string, std::string> args;
    if (argc != 9) throw std::runtime_error("参数: --port N --fixture FILE --requests FILE --ready FILE");
    for (int i = 1; i < argc; i += 2)
        if (!args.emplace(argv[i], argv[i + 1]).second) throw std::runtime_error("重复参数");
    std::size_t consumed = 0;
    const int port = std::stoi(args.at("--port"), &consumed);
    if (consumed != args.at("--port").size() || port <= 0 || port > 65535)
        throw std::runtime_error("非法端口");
    std::ifstream input(args.at("--fixture"));
    if (!input) throw std::runtime_error("无法读取 OPC UA fixture");
    const auto fixture = Json::parse(input);
    Observation observation;
    observation.output.open(args.at("--requests"));
    if (!observation.output) throw std::runtime_error("无法打开节点读取日志");
    // contexts 必须比服务器存活更久，避免 server 析构仍引用失效的数据源。
    std::vector<std::unique_ptr<Node>> contexts;
    ConfigOwner configuration;
    good(UA_ServerConfig_setMinimal(&configuration.value, static_cast<UA_UInt16>(port), nullptr), "配置服务器");
    auto& config = configuration.value;
    // 默认最小配置可能监听全部接口；测试明确限定本机回环，不暴露模拟服务。
    UA_Array_delete(config.serverUrls, config.serverUrlsSize, &UA_TYPES[UA_TYPES_STRING]);
    config.serverUrls = nullptr;
    config.serverUrlsSize = 0;
    config.serverUrls = static_cast<UA_String*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_STRING]));
    if (!config.serverUrls) throw std::bad_alloc();
    config.serverUrlsSize = 1;
    const auto endpoint = "opc.tcp://127.0.0.1:" + std::to_string(port);
    config.serverUrls[0] = UA_STRING_ALLOC(endpoint.c_str());
    if (!config.serverUrls[0].data) throw std::bad_alloc();
    ServerOwner server;
    server.value = UA_Server_newWithConfig(&config); // 转移配置所有权并清空 config。
    if (!server.value) throw std::runtime_error("创建 OPC UA 服务器失败");
    const auto ns = UA_Server_addNamespace(server.value, "urn:industrial-gateway:e2e:opcua");
    if (ns != 2) throw std::runtime_error("测试命名空间索引不是预期的 2");
    for (const auto& device : fixture.at("devices"))
    {
        if (device.at("protocol") != "opcua") continue;
        for (const auto& definition : device.at("nodes"))
        {
            auto node = std::make_unique<Node>();
            node->device_id = device.at("device_id").get<std::int64_t>();
            node->node_id = definition.at("node_id").get<std::string>();
            node->observation = &observation;
            fill(*node, definition);
            // 先交给容器拥有，即使 addNode 中途失败，析构服务器时 context 仍有效。
            contexts.push_back(std::move(node));
            addNode(server.value, *contexts.back());
        }
    }
    if (contexts.empty()) throw std::runtime_error("没有测试节点");
    good(UA_Server_run_startup(server.value), "启动 OPC UA 服务器");
    server.started = true;
    observation.recording = true; // 不把建节点过程中内部读取当作网关请求。
    {
        std::ofstream ready(args.at("--ready"));
        ready << "ready\n";
        ready.flush();
        if (!ready) throw std::runtime_error("无法写入服务器就绪标记");
    }
    while (!stopping && !observation.failed) UA_Server_run_iterate(server.value, true);
    if (observation.failed) throw std::runtime_error("节点读取回调/日志记录失败");
    observation.recording = false;
    good(UA_Server_run_shutdown(server.value), "停止 OPC UA 服务器");
    server.started = false;
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, stopSignal);
    std::signal(SIGTERM, stopSignal);
    try { return run(argc, argv); }
    catch (const std::exception& error)
    { std::cerr << "OPC UA 测试服务器失败: " << error.what() << '\n'; return 1; }
}
