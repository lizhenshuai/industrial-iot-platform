# Modbus RTU 协议适配

协议标识为 `modbus_rtu`。采用现有编译期集成、工厂注册、运行时创建实例的方式，
与 Modbus TCP 共用配置控制、采集调度、组包、队列、SQLite 和 MQTT 上报链路。
本适配面向 Linux 本地串口及 USB 转串口，不包含 RTU over TCP。

## MQTT 配置示例

下发 Topic：`v1/gateways/{gateway_id}/config`。
消息是网关的完整目标配置快照；实际发送时须包含仍要保留的其他连接。

```json
{
  "schema_version": 1,
  "request_id": "rtu-config-001",
  "connections": [
    {
      "connection_id": "modbus_rtu_001",
      "protocol": "modbus_rtu",
      "config": {
        "connection": {
          "device": "/dev/ttyUSB0",
          "baud_rate": 9600,
          "parity": "N",
          "data_bits": 8,
          "stop_bits": 1,
          "serial_mode": "default",
          "response_timeout_ms": 1000,
          "recovery_delay_ms": 1000,
          "inter_request_delay_ms": 0
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
                "byte_order": "high_byte_first",
                "word_order": "high_word_first",
                "interval_ms": 5000,
                "scale": 1.0,
                "offset": 0.0
              },
              {
                "property": "speed",
                "address": 2,
                "register_type": "holding",
                "data_type": "uint16",
                "interval_ms": 5000
              }
            ]
          },
          {
            "device_id": 10002,
            "unit_id": 2,
            "points": [
              {
                "property": "pressure",
                "address": 0,
                "register_type": "input",
                "data_type": "uint16",
                "interval_ms": 5000,
                "scale": 0.1
              }
            ]
          }
        ]
      }
    }
  ]
}
```

该示例由一个采集线程依次读取从站 1 和 2；从站 1 的两个点位用一次读取覆盖地址 0..2。
地址、数据类型和串口参数只是示例，必须按设备实际寄存器表修改。
建议把 `device` 改为 `/dev/serial/by-id/` 下的稳定路径，避免 USB 插拔后设备编号变化。

## 串口字段

| 字段 | 默认值 | 约束及含义 |
|---|---|---|
| `device` | 必填 | 本机串口绝对路径；允许设备暂未插入 |
| `baud_rate` | `9600` | 1200、2400、4800、9600、19200、38400、57600、115200 |
| `parity` | `"N"` | `N` 无校验、`E` 偶校验、`O` 奇校验 |
| `data_bits` | `8` | 固定为 8 |
| `stop_bits` | `1` | 1 或 2，需匹配从站 |
| `serial_mode` | `"default"` | `default`、`rs485`、`rs232`，含义见下文 |
| `response_timeout_ms` | `1000` | 1..60000；单次请求等待完整响应的超时 |
| `recovery_delay_ms` | `1000` | 1..60000；打开串口及异常响应后的隔离等待 |
| `inter_request_delay_ms` | `0` | 0..1000；请求间最小等待，实际取它与协议帧间隔的较大值 |

`default` 保留内核串口模式，适用于自动收发方向控制的 USB 转 RS485 转换器。
`rs485` / `rs232` 显式设置内核 RS485 启用标志，要求串口驱动支持 `TIOCGRS485/TIOCSRS485`；
不支持时连接失败并记录原因，不会静默降级。显式模式仅修改启用标志，保留内核现有方向极性及延时；
关闭连接时尽力恢复原模式。第一版不配置软件 RTS 方向控制及自定义 RTS 极性、延时。
波特率、校验和停止位须由同一串口上的全部从站共同支持。

## 设备、点位及任务

`devices` 和每个设备的 `points` 都不能为空。同连接内 `device_id` 和 `unit_id` 各自不能重复，
`unit_id` 范围为 1..247，不允许使用广播地址采集。

点位必填 `property` 和零基 `address`，不要填写手册的 4xxxx 引用编号。
其余字段缺省为 `holding`、`uint16`、`high_byte_first`、`high_word_first`、1000ms、scale=1、offset=0。
支持 `holding` / `input`，数据类型支持 int16/uint16/int32/uint32/int64/uint64/float32/float64；
字节序可选 `high_byte_first` / `low_byte_first`，字序可选 `high_word_first` / `low_word_first`。

同一设备、同一周期必须使用相同寄存器类型，连续覆盖范围不能超过 125 个寄存器，
所有点位占用范围须在 0..65535 内；间隙地址也会读取，不自动拆分。
不同周期生成不同任务和遥测包。同设备在本轮任务内整体成功或失败，解码失败不提交部分点位。
周期必须为正整数；实际请求耗时超过周期时沿用现有调度器跳过错过周期的规则。

配置解析支持 MQTT JSON 和静态 YAML，两种入口使用相同校验。
静态配置的 `config_file` 指向内容为上述 `config` 对象的 YAML 文件，包含 `connection`、`devices`，
不包含外层 `schema_version`、`request_id`、`connections`。静态文件接口仅供独立调用方使用；
当前主程序采用动态模式，主 YAML 必须保持 `connections: []`，详见 [配置指南](配置指南.md)。

## 运行与恢复

- 一个串口对应一个 Worker；总线上的多个从站依次读取。驱动本身不创建线程。
- 正常请求间保留帧间隔：低波特率按实际串口字符长度计算 3.5 字符，高波特率至少 1.75ms。
- 超时、CRC 或残帧错误：当前设备失败，随后持续丢弃隔离窗口内的数据，确认总线静默后再执行下一个原定请求。
  不重排设备、不无限重试本次请求。持续噪声导致恢复超时后关闭串口，交由 Worker 重连。
- 完整的 Modbus 异常响应只影响当前设备；串口 I/O 故障则释放连接，本任务余下设备返回失败。
- 打开串口后也执行一次恢复等待。`connect()` 成功只代表串口可用；配置 ACK `Applied` 不保证从站在线。
- 动态修改频率、设备、点位或串口参数，沿用现有整连接停止、替换流程；本地 SQLite 仍保存整个网关的一条 JSON 配置。

RTU 没有事务 ID，有限恢复窗口不能保证排除任意延迟的旧响应。应按设备最坏响应时间设置
`response_timeout_ms` 和 `recovery_delay_ms`；超时及恢复等待会延长同总线后续设备的采集时间。

端点识别在设备存在时使用字符设备号，未出现时使用规范路径；离线别名不一定能在准备阶段判为重复。
实际打开时另有进程内设备号占用检查、`flock` 和串口独占设置，避免本程序重复使用同一串口。
这些机制不能排除事先打开端口或具有管理员权限的非合作程序，部署时仍须避免其他串口主站同时使用该总线。
需要 Linux 的 `/proc/self/fd`、终端独占 ioctl 和串口读写权限。

本次未修改共享队列及 ConnectionWorker。队列满时单连接停止可能阻塞的既有限制仍然存在，
不能据此认为配置替换已经具备随时完成的保证。

## 文件及构建

`src/drivers/modbus_rtu/` 包含配置模型、解析器、同步驱动和工厂注册，共七个头文件/实现文件。
内置注册入口和 CMake 已接入，复用已有 libmodbus、yaml-cpp、nlohmann/json，不新增第三方包。

只进行编译检查，不执行单元测试、集成测试或真实串口采集。硬件收发方向、设备响应、拔插恢复等仍需后续联调。

实现参考：[libmodbus 3.1.6 RTU 源码](https://github.com/stephane/libmodbus/blob/v3.1.6/src/modbus-rtu.c)、
[Linux 串口独占说明](https://man7.org/linux/man-pages/man2/TIOCEXCL.2const.html)。
