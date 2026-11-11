# Industrial Gateway

使用 C++17 实现的边缘工业网关。网关接收平台下发的采集配置，通过 Modbus TCP、Modbus RTU 或 OPC UA 读取设备数据，先保存到本地 SQLite，再通过 MQTT 上报。网络中断期间继续采集入库，连接恢复后发送尚未确认的数据。

本文档面向当前 `src/main.cpp` 与 `GatewayRuntime` 装配的可执行程序。功能说明依据仓库实现，不代表所有协议均已完成真实设备联调。

## 当前能力

| 能力 | 当前实现 |
|---|---|
| Modbus TCP | 数字 IPv4 地址；保持寄存器、输入寄存器读取；同端点多个 Unit ID 共用采集线程 |
| Modbus RTU | Linux 本地串口；同总线多个从站串行读取，不包含 RTU over TCP |
| OPC UA | OPC TCP 同步 Read；匿名、None 安全策略；支持指定标量类型 |
| 动态配置 | MQTT 下发完整连接快照，校验后保存到 SQLite，再更新运行连接 |
| 配置恢复 | 进程启动时先恢复本地快照，再处理收到的新配置 |
| 数据持久化 | 完整遥测包批量入库；存储失败保留当前批次并重试 |
| 数据上报 | MQTT QoS 1；收到 PUBACK 后删除本地消息；平台需要处理重复消息 |
| 运行管理 | 日志轮转、关键线程故障检测、SIGINT/SIGTERM 关闭 |

当前未接入业务心跳、在线状态上报、设备控制命令、配置历史版本和自动回滚。OPC UA 不支持订阅或安全认证；MQTT 尚无用户名密码、TLS 和遗嘱配置入口。

## 文档导航

| 文档 | 适合查阅的内容 |
|---|---|
| [本地开发与运行指南](docs/本地开发与运行指南.md) | 从获取项目、安装依赖到本地模拟采集、查看第一条遥测的完整步骤 |
| [系统架构](docs/系统架构.md) | 模块职责、线程与队列、配置应用流程、存储和生命周期 |
| [配置指南](docs/配置指南.md) | 本地 YAML 字段、默认值、动态配置结构、Modbus TCP 配置 |
| [MQTT 通信协议](docs/MQTT通信协议.md) | Topic、配置下发、业务 ACK、遥测格式、重试与去重 |
| [Modbus RTU 协议适配](docs/modbus_rtu.md) | 串口参数、设备点位配置、总线恢复和使用限制 |
| [OPC UA 协议适配](docs/opcua.md) | Endpoint、NodeId、类型转换、会话和安全限制 |
| [依赖记录](DEPENDENCIES.md) | 开发环境和已记录的依赖版本 |

首次使用建议先阅读 [本地开发与运行指南](docs/本地开发与运行指南.md)，按顺序准备环境并运行本地示例；查询字段时阅读配置指南，平台开发者继续阅读 MQTT 通信协议，维护采集链路时参阅系统架构及对应协议文档。

## 编译准备

当前面向 Linux，开发环境记录为 Ubuntu 22.04、GCC/G++ 11.4。项目要求 C++17、CMake 3.16 及以上，并通过 CMake / pkg-config 查找已有依赖。

| 依赖 | 用途 |
|---|---|
| spdlog | 日志 |
| yaml-cpp | 本地 YAML 配置解析 |
| SQLite3 | 配置和遥测消息持久化 |
| nlohmann/json 3.10.5 及以上 | 动态 JSON 配置、ACK 与遥测序列化 |
| libmosquitto | MQTT 客户端 |
| libmodbus | Modbus TCP / RTU 采集 |
| open62541 1.5 系列兼容版本 | OPC UA 采集；当前依赖记录为 1.5.10 |
| pkg-config、系统线程库 | 依赖发现与线程支持 |

具体依赖声明见 [cmake/Dependencies.cmake](cmake/Dependencies.cmake)。当前构建会查找全部生产依赖，即使运行配置中没有使用某个协议。项目不自动下载或安装依赖，也不将仓库内 `open62541/` 源码加入构建。依赖缺失时应先确认本机环境；新增或安装依赖须单独确认。

在本目录执行：

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --target industrial_gateway -j2
```

`BUILD_TESTING=OFF` 关闭测试目标及 GoogleTest 查找。以上只编译网关，不执行测试。若已有库安装在非默认前缀，可指定 `CMAKE_PREFIX_PATH`，或为 open62541 指定其实际 `open62541_DIR`。

## 配置与启动

1. 编辑 [config/gateway.yaml](config/gateway.yaml)，设置 `gateway.id`、Broker 地址和 `mqtt.client_id`。仓库示例的 Broker 地址是 `192.168.0.83`，需按实际环境调整。
2. 保持主 YAML 的 `connections: []`。当前可执行程序通过 MQTT 和 SQLite 恢复管理采集连接，非空静态连接列表会导致启动失败。
3. 从项目目录启动：

```bash
./build/industrial_gateway config/gateway.yaml
```

唯一可选参数是主 YAML 路径，省略时使用 `config/gateway.yaml`。日志和数据库的相对路径以**进程工作目录**为基准，默认分别为 `logs/gateway.log` 和 `data/gateway.db`；父目录会尝试自动创建，运行用户须有写入权限。

首次启动且本地没有配置记录时，不创建采集连接。平台需向 `v1/gateways/{gateway_id}/config` 下发完整快照；消息示例见 [配置指南](docs/配置指南.md)，发送方式见 [MQTT 通信协议](docs/MQTT通信协议.md)。收到 `Applied` 仅表示配置应用完成，设备采集是否成功需结合遥测状态和日志判断。

设置 `mqtt.enabled: false` 后，网关仍恢复本地快照并采集入库，但不接收新配置或发布数据；若数据库中也没有快照，则保持无采集连接状态。

前台运行时使用 `Ctrl+C`，服务管理器可发送 SIGTERM。正常关闭成功返回 `EXIT_SUCCESS`；启动失败或检测到运行故障，在清理完成后返回 `EXIT_FAILURE`。当前未提供 systemd 服务或进程自动重启配置。

## 工程目录

```text
industrial-gateway/
├── CMakeLists.txt
├── cmake/                       # 依赖与编译选项
├── config/gateway.yaml          # 本地基础配置
├── include/industrial_gateway/  # 公共配置、领域模型、应用端口
├── src/
│   ├── main.cpp                 # 可执行程序入口
│   ├── bootstrap/               # 装配与全局生命周期
│   ├── application/             # 配置、连接、调度、存储消费、发布
│   ├── drivers/                 # Modbus TCP、Modbus RTU、OPC UA
│   └── infrastructure/          # YAML/JSON、队列、SQLite、MQTT、日志
├── docs/                        # 架构、配置和通信文档
└── tests/                       # 独立测试任务使用
```

根目录的 `main.cpp` 不参与当前网关可执行目标的构建。

## 使用边界

- 断网补传针对已持久化的消息；进程异常退出时，内存队列及未提交批次不保证保留。
- PUBACK 表示 Broker 对 MQTT 发布的确认，不表示平台业务数据已入库；平台应按网关与 `message_id` 去重。
- 共享采集队列满时，单连接停止、删除或替换可能阻塞。当前没有针对该问题的实现变更。
- SQLite 持续写入失败时，存储线程会保留批次重试；全局关闭不保证固定时间完成。
- 线程健康检查不等同于设备在线检查，也不能检测所有线程卡住的情况。

更完整的故障行为见 [系统架构](docs/系统架构.md)。
