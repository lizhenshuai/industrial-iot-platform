# MQTT 通信协议

本文定义当前网关与平台之间已接入的 MQTT 消息：配置下发、配置结果 ACK 和遥测上报。用于平台对接及排查消息处理结果。网关本地连接参数见 [配置指南](配置指南.md)，线程、队列和持久化实现见 [系统架构](系统架构.md)。

## 1. 身份与连接约定

- Topic 前缀固定为 `v1/gateways/{gateway_id}`，`gateway_id` 来自主 YAML 的 `gateway.id`。
- 网关 ID 必须是非空、非全空白字符串，不含 `/`、`+`、`#` 和 NUL，作为单个 Topic 层级使用。
- `mqtt.client_id` 是 MQTT 连接身份，不参与业务 Topic 构造，也不自动跟随 `gateway.id` 改变。
- 消息体使用 UTF-8 JSON。JSON 对象字段顺序没有业务含义；状态字符串和字段名区分大小写。
- 当前客户端使用 clean session；连接恢复后重新提交已登记的订阅，不依赖 Broker 保存离线会话消息。配置订阅应结合平台保存目标状态和重发机制使用。
- 当前程序没有 MQTT 用户名密码、TLS 或遗嘱配置入口。Broker 必须允许现有连接方式及所需 Topic 权限。

普通网络断线按约 1、2、4、8、16、30 秒退避重连，最大间隔 30 秒；连接成功后重置退避。不可恢复的客户端故障会报告运行错误，不保证无限重连。

## 2. Topic 一览

方向以网关为参照。

| Topic | 方向 | QoS | Retain | 当前用途 |
|---|---|---|---|---|
| `v1/gateways/{gateway_id}/config` | 下行 | 网关请求订阅 QoS 1；建议平台发布 QoS 1 | 平台可使用，见第 4 节 | 全部采集连接的目标快照 |
| `v1/gateways/{gateway_id}/config/ack` | 上行 | 固定 1 | false | 配置处理业务结果 |
| `v1/gateways/{gateway_id}/telemetry` | 上行 | 固定 1 | false | 持久化后的完整遥测包 |

Broker 若只授予配置订阅 QoS 0，网关会记录警告，但继续接受消息。订阅调用成功只表示订阅意图已登记，订阅是否得到 Broker 确认需看 SUBACK 对应日志。

以下名称只在 `MqttTopics` 中预留，当前 Runtime 没有对应消息处理或发送链路，**不能作为已开放接口使用**：

| 预留 Topic 后缀 | 规划用途 |
|---|---|
| `/heartbeat` | 业务心跳 |
| `/availability` | 网关在线/离线状态 |
| `/commands` | 控制命令 |
| `/commands/ack` | 命令执行结果 |

MQTT 协议 Keep Alive 不会产生 `/heartbeat` 业务消息。

## 3. 配置下发消息

Topic：`v1/gateways/{gateway_id}/config`。

```json
{
  "schema_version": 1,
  "request_id": "config-request-001",
  "connections": [
    {
      "connection_id": "modbus_tcp_001",
      "protocol": "modbus_tcp",
      "config": {
        "connection": {
          "host": "192.168.0.10",
          "port": 502
        },
        "devices": [
          {
            "device_id": 10001,
            "unit_id": 1,
            "points": [
              {
                "property": "temperature",
                "address": 0,
                "register_type": "holding",
                "data_type": "float32",
                "interval_ms": 5000
              }
            ]
          }
        ]
      }
    }
  ]
}
```

公共字段规则：

| 字段 | 类型 | 必填与含义 |
|---|---|---|
| `schema_version` | 整数 | 必填，固定 `1`，是结构版本 |
| `request_id` | 字符串 | 必填，非空、非全空白、不含 NUL，用于关联 ACK |
| `connections` | 数组 | 必填，代表网关全部目标连接 |
| `connection_id` | 字符串 | 必填，非空、非全空白、不含 NUL，在当前快照内唯一 |
| `protocol` | 字符串 | 必填，目前为 `modbus_tcp`、`modbus_rtu`、`opcua` |
| `config` | 对象 | 必填，协议专属配置，见 [配置指南](配置指南.md) |

动态消息拒绝未知字段、重复对象键、JSON 注释及不符合字段类型的值。`config_version` 不受支持；整数配置不能写成 `"1"`、`1.0` 或 `true`。

每条配置都是全量快照，不是局部补丁。例如已有 A、B 两个连接，本次只发送 A，表示删除 B。若只想修改 A，应发送修改后的 A 和需要保留的 B。

清空全部采集连接使用：

```json
{
  "schema_version": 1,
  "request_id": "clear-connections-001",
  "connections": []
}
```

缺少 `connections`、传 `null` 或发送空载荷都不是清空采集操作。清空连接也不会删除已入库的遥测积压。

## 4. 顺序、重发与 retained

网关按消息入队顺序串行处理配置，不比较业务版本。`request_id` 只用于关联结果，不作为“已经处理过”的去重键。同 ID 请求可以再次校验、保存和应用。

处理顺序是：解析与准备全部连接 → 保存原始 JSON 到 SQLite → 更新运行连接 → 将结果放入 ACK 队列。收到 MQTT 传输确认不代表这些步骤已完成。

平台可以使用 retained 消息保存当前目标快照，便于网关重新订阅时接收。网关对 retained 与实时消息采用相同处理方式，不会根据 retained 标志忽略或降低优先级。

因此平台需要保持 Broker retained 内容与当前目标一致。如果先 retained 发布旧配置 A，之后仅非 retained 发布新配置 B，网关重连时可能再次收到 A 并应用。删除 Broker 的 retained 记录也不是清空网关连接；清空连接应发送上节的有效空快照。

建议平台在发布前订阅 ACK，为每次业务配置变更生成可追踪的 `request_id`，保存当前完整目标。请求超时后重发**当前目标快照**，避免旧请求的重试覆盖新配置。网关不提供统一的业务 ACK 截止时间：读取恢复、队列积压和连接停止都可能延迟处理，满队列停止问题还可能造成无限等待。

## 5. 配置结果 ACK

Topic：`v1/gateways/{gateway_id}/config/ack`。

成功示例：

```json
{
  "request_id": "config-request-001",
  "status": "Applied",
  "error_message": ""
}
```

关联不到合法请求 ID 的拒绝示例，错误文本仅为示意：

```json
{
  "request_id": null,
  "status": "Rejected",
  "error_message": "root: JSON 语法错误，字节位置 2"
}
```

| 字段 | 类型 | 含义 |
|---|---|---|
| `request_id` | 字符串或 null | 可提取的合法请求 ID；不能可靠提取时为 null |
| `status` | 字符串 | `Applied`、`Rejected`、`Failed` |
| `error_message` | 字符串 | Applied 时为空；其他状态给出非空原因 |

| 状态 | 含义 | 平台应如何理解 |
|---|---|---|
| `Applied` | 整份目标快照已保存且应用完成 | 采集连接更新已完成；不表示设备在线或首次读取成功 |
| `Rejected` | 消息校验或协议准备失败 | 本次配置不保存，运行连接不变；需修正输入 |
| `Failed` | 保存、应用或执行异常 | 可能尚未改动，也可能部分更新；结合错误和网关日志判断 |

JSON 语法和重复键检查成功后，解析器独立提取合法 `request_id`。所以其他字段非法时仍可能返回该 ID；语法错误、重复键、ID 缺失或非法时返回 null。错误文本是诊断信息，当前没有稳定的机器错误码，平台不宜靠完整文本匹配业务分支。

保存失败不会启动候选连接。应用阶段失败可能已经停止旧连接或启动部分新连接，网关不会自动回滚，目标 JSON 仍保存在 SQLite，并报告运行故障等待主线程关闭。故障 ACK 会尝试入队，但不保证在退出前发送完成。

ACK 使用内存队列，发送失败或确认超时保留队首重试，PUBACK 后出队。以下情况平台可能收不到 ACK：配置消息未能入队、ACK 队列已满或关闭、进程重启或故障退出。成功入库和应用不能保证业务 ACK 送达，ACK 本身也可能重复。启动恢复本地配置不会重发历史 ACK。

## 6. 遥测上报

Topic：`v1/gateways/{gateway_id}/telemetry`。

```json
{
  "message_id": "example-packet-001",
  "timestamp_ms": 1794355200000,
  "devices": [
    {
      "device_id": 10001,
      "status": "Success",
      "records": [
        { "property": "temperature", "value": 23.5 },
        { "property": "speed", "value": 1500 }
      ]
    },
    {
      "device_id": 10002,
      "status": "Failure",
      "records": []
    }
  ]
}
```

`message_id` 示例仅用于展示字段，平台应把实际值作为不透明字符串保存，不依赖其内部格式。

| 字段 | 类型 | 含义 |
|---|---|---|
| `message_id` | 字符串 | 业务包标识；同一已持久化包重发时保持不变 |
| `timestamp_ms` | int64 整数 | 任务完成时网关生成的 UTC Unix 毫秒时间 |
| `devices` | 数组 | 本次任务涉及的设备结果 |
| `devices[].device_id` | int64 整数 | 配置中的平台设备 ID |
| `devices[].status` | 字符串 | 正常链路为 `Success` 或 `Failure` |
| `devices[].records` | 数组 | 本设备在当前任务中的点位结果 |
| `records[].property` | 字符串 | 配置中的点位名称 |
| `records[].value` | 布尔、整数、浮点数或字符串 | 采集值，保留领域值类型 |

当前遥测 JSON 没有 `gateway_id`、`connection_id`、`protocol`、`request_id` 或 `schema_version` 字段。网关身份从 Topic 获取，设备及属性语义由平台配置解释。不要要求消息体包含尚未实现的字段。

一次任务生成一个完整包，通常包含同一连接、同一周期的设备结果。不同周期分别组包，平台不能假设每个包都有设备的全部属性。任务按设备串行执行，包时间戳不是所有设备同时采样的证明，也不是 OPC UA 节点的 SourceTimestamp 或 ServerTimestamp。

正常采集链路中，一个设备在本次任务内整体成功或失败：`Success` 包含本任务所需全部点位；`Failure` 的 `records` 为空。具体失败原因写入网关日志，标准遥测设备对象没有 `error_message` 字段。该状态表示本次采集结果，不是持续在线状态。

### 6.1 数值和字符串

领域值支持 bool、int64、uint64、double 和 UTF-8 字符串。JSON 不额外携带类型标签，整数按数值输出，不转换成字符串。

平台解析器应支持 64 位整数：尤其在 JavaScript 默认 Number 场景，超过安全整数范围的 ID 或采集值可能丢失精度。默认缩放保留驱动的整数类型；配置非默认 `scale`/`offset` 后数值可能变为 double，需结合点位配置解释。

正常采集结果不接受 NaN、Infinity 等非有限浮点数。字符串经 JSON 转义，不能依赖手工拼接或按逗号拆分载荷。

### 6.2 序列化诊断扩展

正常组包器会拒绝不合法结果；存储序列化层还对异常输入提供防御性诊断，尽量保留同包其他数据。这些扩展不代表正常设备失败的标准格式。

| 异常位置 | 可能出现的内容 |
|---|---|
| 点位值不能正常序列化 | `value: null`、`serialization_error`、`value_serialization_error`；非有限数还附带 `value_raw` |
| 未知设备状态 | `status: "Unknown"`、`status_code`、`serialization_error` |
| Failure 设备错误地携带点位 | 保留记录并附带设备级 `serialization_error` |
| 字符串包含非法 UTF-8 | 用 U+FFFD 替换非法字节，并增加包级 `serialization_error` |

平台遇到这些字段应将其作为诊断数据处理，不能把 `null` 当成有效的零值，也不应丢弃同包其他合法设备数据。

## 7. 确认、补传与去重

三种“成功”需要分别处理：

| 信号 | 确认的范围 | 未确认的范围 |
|---|---|---|
| 配置发布的 MQTT 确认 | MQTT 传输这一层的处理 | 网关业务入队、落库、应用 |
| 业务 ACK `Applied` | 网关保存并应用了目标配置 | 设备在线、首次采集成功、未来运行不出错 |
| 遥测发布的 PUBACK | Broker 对网关 QoS 1 发布的确认 | 平台消费者处理完成或业务数据库提交 |

遥测路径为：完整包入 SQLite → 按内部 ID 升序读取 → 发布 → 收到 PUBACK → 删除对应 SQLite 记录。单包确认等待时间为 5 秒，失败后约 1 秒重试；这些值当前是代码常量。没有 PUBACK 时保留记录，不跳过失败包继续本批后续遥测。

确认超时不会撤销已经交给 MQTT 客户端的旧发送；确认后、SQLite 删除前的崩溃也可能导致重发。因此平台应以 **`(gateway_id, message_id)`** 作为消息去重依据，避免同包重复执行入库或业务副作用。不要用设备 ID 加时间戳代替包 ID，因为不同任务可能产生相同毫秒时间的包。

网关补传会使用存储时的原始 JSON 和包时间戳。重连后看到较早的 `timestamp_ms` 是正常积压发送行为。网关没有遥测业务 ACK 下行 Topic，不能依赖“平台入库成功后再通知网关删除”的交互。

已入库消息会跨进程重启保留；内存中的采集包、未提交批次、配置命令和 ACK 队列不具备同等持久化保证。数据库保留策略和容量上限目前也未开放配置。

## 8. 手工对接示例

以下命令仅供需要联调时手工使用，不由文档维护过程自动执行。要求已有 Broker 和 `mosquitto_sub` / `mosquitto_pub` 工具；示例使用本机 Broker，按实际环境修改地址。

先在一个终端订阅结果与遥测：

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -q 1 -v \
  -t 'v1/gateways/industrial-gateway-001/config/ack' \
  -t 'v1/gateways/industrial-gateway-001/telemetry'
```

把第 3 节的完整快照保存为 `/tmp/collection-config.json`，确认示例设备参数已替换，再在另一终端发布：

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -q 1 -r \
  -t 'v1/gateways/industrial-gateway-001/config' \
  -f /tmp/collection-config.json
```

这里 `-r` 表示将该目标快照设为 Broker 的 retained 配置。之后修改目标时保持 retained 内容同步。发布命令成功后仍应等待匹配 `request_id` 的业务 ACK，并观察遥测设备状态；首次读取可能在 ACK 之前或之后被平台看到，不能依赖两类 Topic 的先后关系。

## 9. 常见对接问题

| 现象 | 优先检查 |
|---|---|
| 能连 Broker，但没有配置 ACK | Topic 是否使用正确 `gateway.id`、SUBACK、配置是否入队、恢复是否阻塞、ACK 是否积压 |
| 收到 Rejected | JSON 类型、未知字段、重复键、协议字段、重复连接或协议端点 |
| 收到 Applied，但设备 Failure | 设备地址、权限、寄存器/NodeId、类型和超时；查看采集日志 |
| 重连后配置变回旧值 | Broker retained 是否仍是旧目标，是否重发了过期快照 |
| 遥测重复 | 按包 ID 去重；检查确认超时或确认后删除前重启等情况 |
| 没有 heartbeat 或 availability | 当前仅预留名称，未实现上报 |

实现依据包括 [Topic 定义](../src/application/messaging/MqttTopics.hpp)、[配置解析器](../src/infrastructure/config/CollectionConfigParser.cpp)、[ACK 序列化](../src/application/messaging/ConfigAckSerializer.cpp)、[遥测序列化](../src/application/ingestion/StorageWorker.cpp) 和 [发布线程](../src/application/publishing/MqttPublisherWorker.cpp)。
