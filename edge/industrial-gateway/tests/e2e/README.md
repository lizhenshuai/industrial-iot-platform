# 网关进程全链路测试

本目录的脚本启动真正的 `industrial_gateway` 可执行文件，经 MQTT 下发配置，
由 TCP/RTU 驱动读取模拟设备，经业务队列、SQLite 和 MQTT 发布后核对最终数据。
测试不会直接往业务队列塞数据，也不会用模拟组件替代网关的 Worker、存储或发布模块。

当前只完成脚本编写和 Python/JSON 语法检查，**尚未执行任何场景**。
脚本不自动构建、不安装或下载依赖，不自动加入 CTest；需显式运行。

## 环境要求

- Linux，允许本机回环 TCP 监听、创建 PTY，`/proc/self/fd` 可用。
- 已编译且包含 TCP、RTU 内置注册的 `industrial_gateway`。
- Python 3.10 或更高，用于控制脚本；模拟器 Python 环境满足其自身包的 Python 版本要求。
- 模拟器环境已安装 **pymodbus 3.16.1** 和 **pyserial**。默认使用 `../simulators/.venv/bin/python`。
  模拟器接口按本项目现有 3.16.1 源码编写，其他版本不会自动兼容或升级。
- `mosquitto` Broker、`mosquitto_pub`、`mosquitto_sub`（支持 `-F` 输出格式和 MQTT 3.1.1 持久会话）。
- `socat`，用于 RTU/双协议场景；单独 TCP 场景不需要它。

缺少依赖时退出码为 2 并说明原因，不会自动安装。检查开发环境时，已发现客户端和 socat，
但常见路径下未发现 Broker `mosquitto`，运行前需自行准备或通过 `--broker` 指定已有可执行文件。
无需启动系统 Mosquitto 服务；脚本自动启动仅监听 127.0.0.1 的独立 Broker。
不使用项目 `config/gateway.yaml` 中的外部 Broker 地址。

## 运行命令

在网关项目根目录执行。先构建生产可执行文件，`BUILD_TESTING=OFF` 不影响本目录的外部脚本，
也避免要求 GoogleTest 或顺带构建其他测试：

```bash
cmake -S . -B build-e2e -DBUILD_TESTING=OFF
cmake --build build-e2e --target industrial_gateway -j2
```

运行全部三个场景：

```bash
python3 tests/e2e/run_gateway_e2e.py \
  --gateway ./build-e2e/industrial_gateway \
  --scenario all
```

分别运行：

```bash
python3 tests/e2e/run_gateway_e2e.py --gateway ./build-e2e/industrial_gateway --scenario tcp
python3 tests/e2e/run_gateway_e2e.py --gateway ./build-e2e/industrial_gateway --scenario rtu
python3 tests/e2e/run_gateway_e2e.py --gateway ./build-e2e/industrial_gateway --scenario both
```

指定依赖路径及报告目录：

```bash
python3 tests/e2e/run_gateway_e2e.py \
  --gateway ./build-e2e/industrial_gateway \
  --sim-python ../simulators/.venv/bin/python \
  --broker /usr/sbin/mosquitto \
  --scenario all \
  --output /tmp/gateway-e2e-results
```

`--broker` 示例路径需按实际安装位置修改。也可指定 `--mqtt-pub`、`--mqtt-sub`、`--socat`。
脚本不会运行 `uv run`、`uv sync` 或 `pip`，避免运行测试时发生隐式下载。

默认每个稳定观察阶段 22 秒，普通条件等待最多 30 秒，补传等待至少允许 60 秒。
预计全量运行约 4～6 分钟，故障时可能因等待超时更久。
可用 `--window 30 --timeout 60` 延长观察及等待，但不会放宽频率误差判定。
退出码：0=所有场景通过，1=断言/运行/清理失败，2=环境或参数问题，130=中断。

## 场景和数据

| 场景 | 连接及设备 | 阶段 |
|---|---|---|
| tcp | 一个 127.0.0.1 TCP 端点，Unit ID 1/2，device_id 10001/10002 | 初次配置 → 更新 → SQLite 重启恢复 |
| rtu | socat 创建一对 PTY，同一总线 Unit ID 1/2，device_id 20001/20002 | 初次配置 → 更新 → SQLite 重启恢复 |
| both | 同一网关同时使用以上 TCP/RTU 连接，共四个设备 | 初次配置 → 更新 → SQLite 重启恢复 → Broker 断线补传 |

三个大场景依次运行，各有独立网关 ID、端口、数据库和 Broker。
每场景先启动 Broker、MQTT 观察者和模拟器，再启动空配置网关；等待网关确认配置订阅成功后才下发。
配置和遥测分别通过：

```text
v1/gateways/{测试gateway_id}/config
v1/gateways/{测试gateway_id}/config/ack
v1/gateways/{测试gateway_id}/telemetry
```

TCP 和 RTU 共用该网关的 telemetry Topic，按不重复的设备 ID 和固定预期值区分。
测试观察者另使用 `/e2e-ready` 标记确认自己的订阅已经可用，不向网关配置 Topic 发送探测配置。

`fixtures/devices.json` 分别保存原始寄存器值和预期工程值，包含缩放、int16 负数、float32、
uint32 低字在前、uint16 低字节在前。Unit ID 1 读取保持寄存器，Unit ID 2 读取输入寄存器。
无随机值，错误的协议、从站、功能码或字节序不会碰巧得到另一设备的正确结果。

初始配置每个设备有两组：

- 1 秒：temperature、signed_value、float_value，连续读取地址 0..5，共 6 个寄存器。
- 5 秒：wide_value、swapped_value，连续读取地址 20..22，共 3 个寄存器。

更新配置将 1 秒组改为 5 秒，删除 signed_value，新增地址 6 的 added_value。
此时同设备所有点位均为 5 秒、同一种寄存器类型，应合并读取地址 0..22，共 23 个寄存器。
不拆成两次读取，也不继续按 1 秒请求。

## 断言与判断边界

1. 空数据库、未下发配置时不应产生 Modbus 请求。
2. 配置 ACK 必须关联正确 request_id 且为 Applied；SQLite 配置表恰好一条 id=1 记录，内容与下发一致。
3. 模拟器记录每次真实收到的 PDU：单调时钟时间、协议、Unit ID、功能码、起始地址、数量。
   检查请求范围、合并方式、频率、重复/过密请求及明显采集停顿。
4. 检查 telemetry 的完整结构、设备集合、Success 状态、属性集合及数值；协议/频率组不能混包。
   每个周期包包含该连接的两个设备。按 message_id 去重，但同 ID 的内容变化直接失败。
5. 频率使用模拟器请求时间，不使用 MQTT 到达间隔。中位周期误差允许 max(0.35 秒, 周期的 20%)，
   另检查数量及最大/最小间隔，默认观察窗口内每组至少有 3 次请求和 3 个完整包。
   系统负载过重或墙钟明显跳变会报告失败，不能据此直接判断硬件性能。
6. 配置更新前已落库的数据允许稍后发送；检查按包内采集时间划分稳定窗口，等待发布流水线送达。
7. 重启时保留原数据库，不再下发配置，所有配置消息均 non-retained。核对重新产生的请求和新包，
   并核对配置原文及保存时间没有被仅恢复操作改写。
8. 双协议场景停止本次创建的 Broker，读取 SQLite 中断线后新生成的两个协议数据，保存准确的
   message_id 和整个包。恢复 Broker 后逐条核对内容不变且最终从 messages 表删除。
   采集持续进行，不要求整个 messages 表清空。观察者使用持久会话，Broker 正常退出保存会话。
9. 给网关 SIGTERM 后要求 15 秒内退出且返回 0；超时清理会判失败，不会用强杀伪装正常停止。

这套场景验证正常软件链路、配置更新、持久化恢复和补传；不包含从站故障注入、磁盘故障、
队列压满、非法配置安全性、压力/性能测试或全部数据类型边界。
RTU 使用 `serial_mode: default` 和 PTY，不验证真实串口波特率时序、RS485 方向控制、电气质量或 USB 拔插。
已有队列满时停止可能阻塞的限制未修改；强制清理仅发生在测试自己的子进程中。

## 报告与清理

默认报告位于 `/tmp/gateway-e2e-随机后缀/`；`--output` 指定的是父目录，每次仍新建子目录。
成功和失败都保留产物，不覆盖、删除已有报告。报告路径不能包含逗号或换行。

```text
gateway-e2e-.../
├── report.json
├── tcp/
├── rtu/
└── both/
    ├── result.json
    ├── failure.txt              # 失败时的异常堆栈
    ├── gateway.yaml
    ├── config-initial.json
    ├── config-updated.json
    ├── gateway.db
    ├── gateway-1.log
    ├── gateway-2.log
    ├── tcp-requests.jsonl
    ├── rtu-requests.jsonl
    ├── mqtt-messages.jsonl
    ├── offline-packets.json
    └── ...                     # Broker/模拟器/socat 日志及会话数据
```

所有子进程单独建立进程组。正常完成、断言失败、Ctrl+C 或 SIGTERM 都尝试清理本次进程，
超时才强杀自己的进程组；不使用 killall、不操作系统服务、不连接现有外部 Broker。
对子进程已知的健康状态采用有限等待。SIGKILL、机器断电等无法被脚本捕获，仍可能留下进程或临时文件。

脚本采用临时端口，端口探测与实际监听间仍存在竞争窗口，若被抢占则报告启动失败。
若容器或沙箱禁止监听/PTY，应在具备这些权限的 Linux 环境运行，不应将权限错误算作测试通过。
