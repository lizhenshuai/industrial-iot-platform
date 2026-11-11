#include "OpcUaConnector.hpp"
#include "OpcUaConfigParser.hpp"

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace industrial_gateway::drivers::opcua
{
namespace
{
using domain::model::CollectionResult;
using domain::model::CollectionTask;
using domain::enums::CollectionStatus;
using domain::types::TelemetryValue;

// open62541 返回的响应和动态 NodeId 都需要 clear；异常路径也必须释放。
template<class T, std::size_t Type>
struct UaValue
{
    T value{};
    UaValue() = default;
    ~UaValue() { UA_clear(&value, &UA_TYPES[Type]); }
    UaValue(const UaValue&) = delete;
    UaValue& operator=(const UaValue&) = delete;
};
using Request = UaValue<UA_ReadRequest, UA_TYPES_READREQUEST>;
using Response = UaValue<UA_ReadResponse, UA_TYPES_READRESPONSE>;
using Client = std::unique_ptr<UA_Client, decltype(&UA_Client_delete)>;

std::string uaError(const std::string& operation, UA_StatusCode code)
{
    std::ostringstream message;
    message << operation << ": " << UA_StatusCode_name(code) << " (0x"
            << std::hex << std::setw(8) << std::setfill('0') << code << ')';
    return message.str();
}

// 只用于服务级错误。节点 DataValue 中的 BadNoCommunication 等质量码可能
// 描述服务器背后的设备离线，并不意味着本客户端与服务器的会话失效。
bool connectionFailure(UA_StatusCode code)
{
    switch (code)
    {
        case UA_STATUSCODE_BADTIMEOUT:
        case UA_STATUSCODE_BADREQUESTTIMEOUT:
        case UA_STATUSCODE_BADCOMMUNICATIONERROR:
        case UA_STATUSCODE_BADSERVERNOTCONNECTED:
        case UA_STATUSCODE_BADNOTCONNECTED:
        case UA_STATUSCODE_BADCONNECTIONCLOSED:
        case UA_STATUSCODE_BADCONNECTIONREJECTED:
        case UA_STATUSCODE_BADSECURECHANNELIDINVALID:
        case UA_STATUSCODE_BADTCPSECURECHANNELUNKNOWN:
        case UA_STATUSCODE_BADSECURECHANNELCLOSED:
        case UA_STATUSCODE_BADSECURECHANNELTOKENUNKNOWN:
        case UA_STATUSCODE_BADSESSIONIDINVALID:
        case UA_STATUSCODE_BADSESSIONCLOSED:
        case UA_STATUSCODE_BADSESSIONNOTACTIVATED:
            return true;
        default:
            return false;
    }
}

TelemetryValue decode(const UA_DataValue& data, const OpcUaPointConfig& point)
{
    if (data.hasStatus && !UA_StatusCode_isGood(data.status))
        throw std::runtime_error(uaError("节点质量不是 Good", data.status));
    if (!data.hasValue || !UA_Variant_isScalar(&data.value) || !data.value.type)
        throw std::runtime_error("节点缺少标量值，或返回了数组");
    const auto& value = data.value;
    TelemetryValue decoded;
    // 精确匹配内置类型，不能把 DateTime、枚举或 ExtensionObject 当普通整数。
    if (value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) decoded = *static_cast<const UA_Boolean*>(value.data) != 0;
    else if (value.type == &UA_TYPES[UA_TYPES_SBYTE]) decoded = std::int64_t{*static_cast<const UA_SByte*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_INT16]) decoded = std::int64_t{*static_cast<const UA_Int16*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_INT32]) decoded = std::int64_t{*static_cast<const UA_Int32*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_INT64]) decoded = std::int64_t{*static_cast<const UA_Int64*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_BYTE]) decoded = std::uint64_t{*static_cast<const UA_Byte*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_UINT16]) decoded = std::uint64_t{*static_cast<const UA_UInt16*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_UINT32]) decoded = std::uint64_t{*static_cast<const UA_UInt32*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_UINT64]) decoded = std::uint64_t{*static_cast<const UA_UInt64*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_FLOAT]) decoded = double{*static_cast<const UA_Float*>(value.data)};
    else if (value.type == &UA_TYPES[UA_TYPES_DOUBLE]) decoded = *static_cast<const UA_Double*>(value.data);
    else if (value.type == &UA_TYPES[UA_TYPES_STRING])
    {
        const auto& text = *static_cast<const UA_String*>(value.data);
        if (!text.data) throw std::runtime_error("不支持 Null String，领域模型没有 null 类型");
        // 必须按显式长度复制，保留内嵌 NUL；JSON 负责转义，而不是按 C 字符串截断。
        auto result = text.length ? std::string(reinterpret_cast<const char*>(text.data), text.length) : std::string{};
        try { (void)nlohmann::json(result).dump(); }
        catch (const nlohmann::json::exception&) { throw std::runtime_error("字符串不是有效 UTF-8"); }
        decoded = std::move(result);
    }
    else throw std::runtime_error("不支持的 OPC UA 标量类型");

    if (const auto* number = std::get_if<double>(&decoded); number && !std::isfinite(*number))
        throw std::runtime_error("浮点值不是有限数值");
    if (point.scale == 1.0 && point.offset == 0.0) return decoded;
    return std::visit([&](const auto& item) -> TelemetryValue
    {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>)
            throw std::runtime_error("布尔和字符串节点不能配置非默认缩放");
        else
        {
            // 与现有 Modbus 约定一致：缩放输出 double，64 位大整数可能损失精度。
            const double scaled = static_cast<double>(item) * point.scale + point.offset;
            if (!std::isfinite(scaled)) throw std::runtime_error("缩放结果不是有限数值");
            return scaled;
        }
    }, decoded);
}

struct DeviceReadPlan
{
    const OpcUaDeviceConfig* device{nullptr};
    std::vector<const OpcUaPointConfig*> points;
};
} // namespace

class OpcUaConnector::Impl
{
public:
    Impl(std::string id, OpcUaConfig config, common::ILogger& logger)
        : connection_id_(std::move(id)), config_(std::move(config)), logger_(logger)
    {
        if (connection_id_.find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
            connection_id_.find('\0') != std::string::npos)
            throw std::invalid_argument("connection_id 不能为空或包含空字符");
        std::string error;
        if (!OpcUaConfigParser{}.validate(config_, error)) throw std::invalid_argument(error);
        endpoint_ = OpcUaConfigParser::normalizeEndpoint(config_.endpoint_url);
    }

    bool stateIsConnected(UA_Client* client) const noexcept
    {
        UA_SecureChannelState channel;
        UA_SessionState session;
        UA_StatusCode status;
        UA_Client_getState(client, &channel, &session, &status);
        return UA_StatusCode_isGood(status) && channel == UA_SECURECHANNELSTATE_OPEN &&
               session == UA_SESSIONSTATE_ACTIVATED;
    }

    bool connect(std::string& error)
    {
        error.clear();
        if (connected_.load()) return true;
        disconnect();
        Client candidate(UA_Client_new(), UA_Client_delete);
        if (!candidate) { error = "分配 OPC UA 客户端失败"; return false; }
        auto* options = UA_Client_getConfig(candidate.get());
        options->timeout = static_cast<UA_UInt32>(config_.timeout.count());
        options->securityMode = UA_MESSAGESECURITYMODE_NONE;
        UA_String_clear(&options->securityPolicyUri);
        options->securityPolicyUri = UA_STRING_ALLOC("http://opcfoundation.org/UA/SecurityPolicy#None");
        if (!options->securityPolicyUri.data) { error = "分配安全策略 URI 失败"; return false; }
        UA_ExtensionObject_clear(&options->userIdentityToken);
        auto* identity = UA_AnonymousIdentityToken_new();
        if (!identity) { error = "分配匿名身份失败"; return false; }
        UA_ExtensionObject_setValue(&options->userIdentityToken, identity, &UA_TYPES[UA_TYPES_ANONYMOUSIDENTITYTOKEN]);
        options->connectivityCheckInterval = 0; // 不增加业务读取之外的周期探测。
        const auto status = UA_Client_connect(candidate.get(), endpoint_.c_str());
        if (!UA_StatusCode_isGood(status) || !stateIsConnected(candidate.get()))
        {
            error = UA_StatusCode_isGood(status) ? "OPC UA 会话未激活" : uaError("OPC UA 连接失败", status);
            return false;
        }
        // 初次握手可能包含端点发现；连接成功后禁用库的自动重连和新会话，
        // 统一由现有 Worker 决定重试时间，不在 Read 内暗中创建替代连接。
        options->noReconnect = true;
        options->noNewSession = true;
        client_ = std::move(candidate);
        connected_.store(true);
        try { logger_.info("OpcUaConnector", "连接已建立: " + connection_id_ + " (" + endpoint_ + ")"); }
        catch (...) {}
        return true;
    }

    void disconnect() noexcept
    {
        connected_.store(false);
        // delete 内含会话关闭和事件循环资源释放，保留配置的有限服务超时。
        client_.reset();
    }

    bool makePlans(const CollectionTask& task, std::vector<DeviceReadPlan>& plans, std::string& error) const
    {
        const auto fail = [&](const std::string& reason)
        { error = "连接 " + connection_id_ + " 的采集任务无效: " + reason; return false; };
        if (task.connection_id != connection_id_) return fail("connection_id 不匹配");
        if (task.interval.count() <= 0 || task.devices.empty()) return fail("周期必须为正，设备不能为空");
        std::size_t expected_devices = 0;
        for (const auto& device : config_.devices)
            if (std::any_of(device.points.begin(), device.points.end(),
                [&](const auto& point) { return point.interval == task.interval; })) ++expected_devices;
        if (task.devices.size() != expected_devices) return fail("必须包含该周期的全部设备");
        std::unordered_set<std::int64_t> ids;
        for (const auto& requested : task.devices)
        {
            if (!ids.insert(requested.device_id).second) return fail("设备重复");
            const auto device = std::find_if(config_.devices.begin(), config_.devices.end(),
                [&](const auto& item) { return item.device_id == requested.device_id; });
            if (device == config_.devices.end()) return fail("设备不存在");
            const auto expected_points = std::count_if(device->points.begin(), device->points.end(),
                [&](const auto& point) { return point.interval == task.interval; });
            if (requested.properties.empty() || requested.properties.size() != static_cast<std::size_t>(expected_points))
                return fail("必须包含设备该周期的全部点位");
            DeviceReadPlan plan;
            plan.device = &*device;
            std::unordered_set<std::string> properties;
            for (const auto& property : requested.properties)
            {
                if (!properties.insert(property).second) return fail("点位重复: " + property);
                const auto point = std::find_if(device->points.begin(), device->points.end(),
                    [&](const auto& item) { return item.property == property && item.interval == task.interval; });
                if (point == device->points.end()) return fail("点位不存在或周期不匹配: " + property);
                plan.points.push_back(&*point);
            }
            plans.push_back(std::move(plan));
        }
        return true;
    }

    CollectionResult readDevice(const DeviceReadPlan& plan)
    {
        CollectionResult result;
        result.device_id = plan.device->device_id;
        if (!connected_.load())
        { result.error_message = "OPC UA 未连接，等待 Worker 重连"; return result; }
        // 处理已到达的断线通知/维护事件；没有后台线程。长周期或队列阻塞时
        // 无法保证会话保活，下次读取发现失效后交由 Worker 重连。
        const auto iterate = UA_Client_run_iterate(client_.get(), 0);
        if (!UA_StatusCode_isGood(iterate) || !stateIsConnected(client_.get()))
        {
            result.error_message = UA_StatusCode_isGood(iterate) ? "OPC UA 会话已失效" : uaError("连接维护失败", iterate);
            disconnect();
            return result;
        }
        Request request;
        auto& req = request.value;
        req.nodesToRead = static_cast<UA_ReadValueId*>(UA_Array_new(plan.points.size(), &UA_TYPES[UA_TYPES_READVALUEID]));
        if (!req.nodesToRead) throw std::bad_alloc();
        req.nodesToReadSize = plan.points.size();
        req.maxAge = 0; // 请求服务器尽可能读取当前值，不宣称是原子设备快照。
        req.timestampsToReturn = UA_TIMESTAMPSTORETURN_NEITHER;
        req.requestHeader.timeoutHint = static_cast<UA_UInt32>(config_.timeout.count());
        for (std::size_t i = 0; i < plan.points.size(); ++i)
        {
            const auto& text = plan.points[i]->node_id;
            const auto status = UA_NodeId_parse(&req.nodesToRead[i].nodeId, UA_STRING(const_cast<char*>(text.c_str())));
            if (!UA_StatusCode_isGood(status))
            { result.error_message = uaError("准备 NodeId 失败", status); return result; }
            req.nodesToRead[i].attributeId = UA_ATTRIBUTEID_VALUE;
        }
        Response response;
        response.value = UA_Client_Service_read(client_.get(), req);
        const auto status = response.value.responseHeader.serviceResult;
        // 在释放 client 前保留响应；响应由独立 RAII 对象拥有。
        if (!stateIsConnected(client_.get()) || connectionFailure(status)) disconnect();
        if (!UA_StatusCode_isGood(status))
        { result.error_message = uaError("Read 服务失败", status); return result; }
        if (response.value.resultsSize != plan.points.size() || !response.value.results)
        { result.error_message = "Read 响应的结果数量与请求不一致"; return result; }
        std::vector<domain::model::TelemetryRecord> records;
        records.reserve(plan.points.size());
        for (std::size_t i = 0; i < plan.points.size(); ++i)
        {
            try { records.push_back({plan.points[i]->property, decode(response.value.results[i], *plan.points[i])}); }
            catch (const std::runtime_error& exception)
            {
                result.error_message = "点位 " + plan.points[i]->property + " (" + plan.points[i]->node_id + "): " + exception.what();
                return result; // 不暴露当前设备已经解码成功的部分点位。
            }
        }
        result.records = std::move(records);
        result.status = CollectionStatus::Success;
        return result;
    }

    bool readPoints(const CollectionTask& task, std::vector<CollectionResult>& results, std::string& error)
    {
        results.clear();
        error.clear();
        std::vector<DeviceReadPlan> plans;
        if (!makePlans(task, plans, error)) return false;
        std::vector<CollectionResult> completed;
        completed.reserve(plans.size());
        for (const auto& plan : plans) completed.push_back(readDevice(plan));
        results = std::move(completed);
        return true;
    }

    const std::string connection_id_;
    const std::string protocol_{"opcua"};
    const OpcUaConfig config_;
    common::ILogger& logger_;
    std::string endpoint_;
    Client client_{nullptr, UA_Client_delete};
    std::atomic<bool> connected_{false};
};

OpcUaConnector::OpcUaConnector(std::string id, OpcUaConfig config, common::ILogger& logger)
    : impl_(std::make_unique<Impl>(std::move(id), std::move(config), logger)) {}
OpcUaConnector::~OpcUaConnector() { disconnect(); }
bool OpcUaConnector::connect(std::string& error) { return impl_->connect(error); }
void OpcUaConnector::disconnect() noexcept { impl_->disconnect(); }
bool OpcUaConnector::isConnected() const noexcept { return impl_->connected_.load(); }
bool OpcUaConnector::readPoints(const CollectionTask& task, std::vector<CollectionResult>& results, std::string& error)
{ return impl_->readPoints(task, results, error); }
const std::string& OpcUaConnector::connectionId() const noexcept { return impl_->connection_id_; }
const std::string& OpcUaConnector::protocolType() const noexcept { return impl_->protocol_; }
const std::string& OpcUaConnector::endpointKey() const noexcept { return impl_->endpoint_; }
} // namespace industrial_gateway::drivers::opcua
