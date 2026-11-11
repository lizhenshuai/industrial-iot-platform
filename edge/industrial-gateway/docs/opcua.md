# OPC UA 协议适配

协议标识为 `opcua`。采用编译期集成、工厂注册、运行时创建实例的方式，复用现有
ConnectionWorker、PollScheduler、TelemetryPacketBuilder、队列、SQLite 和 MQTT 链路。
第一版为匿名、无加密的 OPC TCP 同步 Read 客户端，不使用订阅，不增加业务线程。

## 配置示例

下发 Topic 为 `v1/gateways/{gateway_id}/config`，下面是网关的完整目标快照。
如需保留 Modbus TCP/RTU 连接，须把它们一起放在同一份 `connections` 数组中。

```json
{
  "schema_version": 1,
  "request_id": "opcua-config-001",
  "connections": [
    {
      "connection_id": "opcua_001",
      "protocol": "opcua",
      "config": {
        "connection": {
          "endpoint_url": "opc.tcp://192.168.0.10:4840",
          "timeout_ms": 3000,
          "security_mode": "None",
          "authentication": "anonymous"
        },
        "devices": [
          {
            "device_id": 30001,
            "points": [
              {
                "property": "temperature",
                "node_id": "ns=2;s=Device1.Temperature",
                "interval_ms": 1000,
                "scale": 1.0,
                "offset": 0.0
              },
              {
                "property": "running",
                "node_id": "ns=2;i=1002",
                "interval_ms": 5000
              }
            ]
          },
          {
            "device_id": 30002,
            "points": [
              {
                "property": "speed",
                "node_id": "ns=2;s=Device2.Speed",
                "interval_ms": 1000
              }
            ]
          }
        ]
      }
    }
  ]
}
```

服务器地址、命名空间及 NodeId 必须按实际设备修改。配置准备只校验语法及本地约束，
不探测服务器是否在线、节点是否存在或允许访问。ACK `Applied` 不代表首次采集已成功。
静态工厂也支持 YAML，内容为上述 `config` 对象的 `connection` 和 `devices`；
当前主程序的动态入口仍要求主配置中 `connections: []`。

## 字段和校验

| 字段 | 默认值 | 约束 |
|---|---|---|
| `connection.endpoint_url` | 必填 | `opc.tcp://主机[:端口][/路径]`，最长 4096 字节 |
| `connection.timeout_ms` | 3000 | 1..60000，客户端操作超时；不是整个多设备任务的总超时 |
| `connection.security_mode` | `None` | 第一版仅接受此值，SecurityPolicy 固定为 None |
| `connection.authentication` | `anonymous` | 第一版仅接受匿名，不支持用户名密码或证书身份 |
| `device_id` | 必填 | int64 平台逻辑分组，同一连接内唯一，不是协议设备地址 |
| `property` | 必填 | 非空，同一设备内唯一 |
| `node_id` | 必填 | `[ns=N;]i=数字` 或 `[ns=N;]s=非空字符串`，最长 4096 字节 |
| `interval_ms` | 1000 | 正整数毫秒 |
| `scale` / `offset` | 1 / 0 | 有限数值；非默认缩放仅适用于数值节点 |

JSON/YAML 共用语义校验，拒绝未知字段、重复键和显式 null。
JSON 整数配置不能写为字符串、布尔值或小数。设备及点位数组均不能为空。
同一设备同一周期最多 1024 个点位，这是驱动的资源上限，不是 OPC UA 标准上限。
服务器可能设置更小的 Read 数量限制；返回 BadTooManyOperations 时该设备失败，不自动拆分请求。

Endpoint 支持数字 IPv4、方括号包裹的 IPv6 和 ASCII DNS 主机名，缺省端口为 4840。
不接受 URL 用户信息、查询参数、片段、空白或原始非 ASCII 字符。
端点标识规范化主机大小写、IP 写法和端口；空路径与 `/` 相同，其他路径区分大小写并保留。
同一规范端点不允许配置多个采集连接，应将设备合并在一个连接内。
规范化不进行 DNS 查询，因此不能识别不同域名或域名与 IP 指向同一服务器的别名关系。
DNS 解析耗时受操作系统影响，`timeout_ms` 不提供覆盖解析、清理和线程退出的硬实时上限。

NodeId 的 namespace 范围为 0..65535，省略时为 0；数字标识范围为 uint32，Null NodeId 被拒绝。
字符串标识按实际文本解释，不作为 BrowsePath；暂不支持 GUID、ByteString NodeId 或 Namespace URI 映射。
服务器须保持命名空间索引稳定，否则重启后的相同 `ns=N` 可能不再指向原命名空间。
不同 property 可以映射到同一节点；仍按各自配置周期和缩放规则读取。

## 读取与转换

一个规范 Endpoint 对应一个 Worker、一个客户端会话和一个采集线程。
工厂按周期生成任务，各设备按配置顺序依次执行；同设备同周期的全部节点使用一次 Read 服务调用，
读取 Value 属性，节点之间不要求连续。不同频率分别调用驱动、分别组包。
Read 使用 `maxAge=0` 请求尽可能新的值，但批量 Read 不是原子快照，也不保证物理传感器同时刷新。

返回结果按请求顺序匹配点位。服务失败、结果数量不符、节点 Bad/Uncertain、无值或转换失败，
均令当前设备本次采集返回 Failure 和空 records；其余设备仍有自己的结果。
错误日志包含点位和 OPC UA 状态码。节点级错误不因名字包含“通信失败”就关闭客户端会话。
网络/会话失效及服务超时则释放连接，当前任务尚未读取的设备返回失败，重连时机由 Worker 决定。

| OPC UA 标量 | 遥测类型 |
|---|---|
| Boolean | bool |
| SByte / Int16 / Int32 / Int64 | int64_t |
| Byte / UInt16 / UInt32 / UInt64 | uint64_t |
| Float / Double | double |
| String | UTF-8 std::string |

默认缩放保留整数精度；非默认缩放输出 double，大整数可能损失精度。
非有限浮点数、缩放溢出、数组、DateTime、ByteString、结构体及自定义类型均不支持。
布尔/字符串应用非默认缩放时返回设备失败。空字符串可上报，Null String 因领域模型不支持 null 而失败；
字符串按长度复制，内嵌 NUL 交给 JSON 转义，无效 UTF-8 在驱动阶段拒绝。
不配置 Modbus 的 data_type、byte_order 或 word_order，按服务器返回的实际内置类型转换。

包时间戳仍由 Worker 在任务完成后生成 UTC Unix 毫秒值，不使用节点 SourceTimestamp 或 ServerTimestamp。
驱动不改写 MQTT 消息结构、队列元素、配置存储表或 message_id 规则。

## 生命周期和限制

客户端连接成功后关闭库的自动重连/自动新建会话，复用现有 Worker 的重连调度。
读取前在当前采集线程处理非阻塞维护事件，同步服务调用也驱动库的事件循环。
没有独立的保活线程；长周期或队列阻塞可能使会话过期，下次调用发现后断开并等待 Worker 重连。
这种情况下可能出现失败采集轮次，不能承诺连接始终在线。

配置变化沿用整连接替换，未修改 ConnectionWorker 和共享队列；队列满时单连接停止可能阻塞的
已知限制保留。客户端操作、清理及全局退出也不构成实时截止时间保证。
底层 open62541 保留其默认诊断输出，驱动的连接成功及设备失败信息进入现有网关日志链路。

第一版只能连接允许匿名、SecurityPolicy None、MessageSecurityMode None 的服务器。
当前安装的 open62541 未启用加密，且驱动会显式限制策略，不会自动降级其他配置。
需要 Sign/SignAndEncrypt、用户名密码、证书身份或信任管理时，应另行扩展依赖能力和配置设计。
本轮不含订阅、浏览发现、历史读取、写节点、方法调用和自动化测试。

## 构建

新增七个驱动文件位于 `src/drivers/opcua/`，由 `RegisterBuiltInConnectors` 注册。
构建目标为 `gateway_opcua`，通过 `find_package(open62541 1.5 CONFIG REQUIRED)` 查找已安装库，
链接 `open62541::open62541`；不编译仓库中的第三方源码，不下载、不安装依赖。
本机适配版本为 open62541 1.5.10。

```bash
cmake -S . -B build-e2e -DBUILD_TESTING=OFF
cmake --build build-e2e --target industrial_gateway -j2
```

安装路径不在 CMake 默认搜索范围时可传 `-Dopen62541_DIR=/实际路径/lib/cmake/open62541`。
部署时也需要可加载的对应运行时动态库。
本轮仅进行整个可执行目标的编译检查，未执行测试或连接 OPC UA 服务器；现有 E2E 仍只覆盖 TCP/RTU。
