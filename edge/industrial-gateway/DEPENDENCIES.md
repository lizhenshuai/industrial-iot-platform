# DEPENDENCIES

本文档记录 `industrial-iot-platform` 项目的开发环境与第三方依赖版本。

目标是让项目在相同环境下可重复构建，并降低依赖升级带来的不确定性。

## 1. 开发环境

| 组件 | 固定/记录版本 | 说明 |
|---|---|---|
| Operating System | Ubuntu 22.04.5 LTS | 当前开发系统 |
| Compiler | GCC / G++ 11.4.0 | 当前编译器 |
| C++ Standard | C++17 | 项目统一使用 C++17 |
| CMake | 3.22.1 | 构建工具 |
| pkg-config | 0.29.2-1ubuntu3 | 第三方库配置查询工具 |

## 2. 第三方依赖

### 2.1 libmodbus

用于工业设备 Modbus TCP 协议通信。

| 依赖 | 软件包 | 锁定版本 | 状态 |
|---|---|---|---|
| Modbus 开发库 | `libmodbus-dev` | `3.1.6-2` | 已执行指定版本安装，待验证 |
| Modbus 运行时库 | `libmodbus5` | `3.1.6-2` | 已执行指定版本安装，待验证 |

### 2.2 Mosquitto MQTT

用于 MQTT 客户端开发，通过 MQTT 协议向 EMQX Broker 发布和订阅消息。

| 依赖 | 软件包 | 锁定版本 | 状态 |
|---|---|---|---|
| MQTT 开发库 | `libmosquitto-dev` | `2.0.11-1ubuntu1.2` | 已执行指定版本安装，待验证 |
| MQTT 运行时库 | `libmosquitto1` | 待验证 | 随开发库依赖安装 |
| MQTT 命令行工具 | `mosquitto-clients` | `2.0.11-1ubuntu1.2` | 已执行指定版本安装，待验证 |

说明：

- `libmosquitto-dev` 提供 C/C++ 开发头文件和链接库。
- `libmosquitto1` 提供运行时动态库。
- `mosquitto-clients` 提供 `mosquitto_pub`、`mosquitto_sub` 测试工具。
- MQTT Broker 使用 EMQX，当前 Ubuntu 环境不需要额外安装 Mosquitto Broker。

### 2.3 SQLite

用于工业网关本地数据持久化，实现数据缓存、异常恢复和断网补发。

| 依赖 | 软件包 | 锁定版本 | 状态 |
|---|---|---|---|
| SQLite 开发库 | `libsqlite3-dev` | `3.37.2-2ubuntu0.8` | 已安装并验证 |
| SQLite 运行时库 | `libsqlite3-0` | `3.37.2-2ubuntu0.8` | 已安装并验证 |
| SQLite 命令行工具 | `sqlite3` | `3.37.2-2ubuntu0.8` | 已安装并验证 |

说明：

- `libsqlite3-dev` 提供 C/C++ 开发头文件和链接库。
- `libsqlite3-0` 提供 SQLite 运行时动态库。
- `sqlite3` 提供数据库命令行操作工具。
- SQLite 用于本地持久化存储，后续根据可靠性要求配置 WAL 和同步级别。

当前 SQLite 版本来自 Ubuntu 22.04（Jammy）官方软件源，已通过 `dpkg-query` 确认实际安装版本。

### 2.4 其他辅助工具

| 依赖 | 软件包 | 版本 | 用途 |
|---|---|---|---|
| TCP 网络测试工具 | `netcat-openbsd` | 待验证 | TCP 端口连通性测试 |

### 2.5 nlohmann/json

用于 MQTT 采集配置快照的 JSON 解析，以及协议配置对象的 JSON 序列化。

| 依赖 | 软件包 | 已确认版本 | 状态 |
|---|---|---|---|
| JSON 头文件库 | `nlohmann-json3-dev` | `3.10.5-2`（库版本 `3.10.5`） | 用户安装；2026-11-10 确认软件包及头文件版本 |

用于公共配置及 Modbus JSON 解析、配置内容比较和 ACK 序列化，公共配置模型不暴露 JSON 库类型。
构建声明已使用 `find_package(nlohmann_json 3.10.5 CONFIG REQUIRED)` 和
`nlohmann_json::nlohmann_json` 目标；libmodbus 使用 `PkgConfig::MODBUS`。
本次配置控制链路修改未编译、未测试，也未执行 CMake 配置；此前单文件检查不代表当前代码已验证。
不使用自动下载或 FetchContent。

### 2.6 open62541

OPC UA 同步采集使用已有安装的 open62541 1.5.10（头文件 `/usr/local/include/open62541`，
库 `/usr/local/lib/libopen62541.so.1.5.10`）。当前安装未启用加密，第一版驱动仅支持匿名及 None 安全策略。
构建使用 `find_package(open62541 1.5 CONFIG REQUIRED)` 和 `open62541::open62541`。
本次没有下载、安装或重新构建该依赖，不使用项目内 `open62541/` 源码目录参与构建。

## 3. 安装命令

### 3.1 libmodbus

```bash
sudo apt install -y \
    libmodbus-dev=3.1.6-2 \
    libmodbus5=3.1.6-2
```

### 3.2 Mosquitto MQTT

```bash
sudo apt install -y \
    libmosquitto-dev=2.0.11-1ubuntu1.2 \
    mosquitto-clients=2.0.11-1ubuntu1.2
```

`libmosquitto1` 由 APT 根据依赖关系自动安装。

### 3.3 SQLite

```bash
sudo apt update

sudo apt install -y \
    sqlite3 \
    libsqlite3-dev
```

### 3.4 其他辅助工具

```bash
sudo apt install -y \
    netcat-openbsd
```

## 4. 版本验证命令

### 4.1 操作系统与编译工具

```bash
# 操作系统
lsb_release -ds

# GCC / G++
g++ --version
gcc --version

# CMake
cmake --version

# pkg-config
pkg-config --version
```

### 4.2 libmodbus

```bash
# 软件包版本
dpkg-query -W -f='${binary:Package}=${Version}\n' \
    libmodbus-dev libmodbus5

# 库版本
pkg-config --modversion libmodbus

# 编译与链接参数
pkg-config --cflags --libs libmodbus
```

### 4.3 Mosquitto MQTT

```bash
# 软件包版本
dpkg-query -W -f='${binary:Package}=${Version}\n' \
    libmosquitto-dev libmosquitto1 mosquitto-clients

# MQTT 开发库版本
pkg-config --modversion libmosquitto

# 编译与链接参数
pkg-config --cflags --libs libmosquitto

# 检查命令行工具
mosquitto_pub --help
mosquitto_sub --help
```

### 4.4 SQLite

```bash
# SQLite 命令行版本
sqlite3 --version

# SQLite 开发库版本
pkg-config --modversion sqlite3

# 软件包实际安装版本
dpkg-query -W -f='${binary:Package}=${Version}\n' \
    sqlite3 libsqlite3-dev libsqlite3-0

# 编译与链接参数
pkg-config --cflags --libs sqlite3
```

### 4.5 查看全部开发依赖

```bash
dpkg -l | grep -E 'lib.*-dev|sqlite3|mosquitto|netcat'
```

## 5. 项目构建约定

- 项目统一使用 C++17。
- 使用 CMake 管理项目构建。
- 第三方依赖应记录准确版本，不依赖系统中未记录的最新版。
- 修改依赖版本前，先在开发环境验证编译、运行和相关测试。
- 依赖升级后，及时更新本文档。
- 生产部署应使用经过验证的依赖版本，不要在生产设备上未经测试直接升级系统依赖。
- 第三方库通过系统开发包安装，使用 `find_package` 或 `pkg-config` 管理编译和链接依赖。

CMake 中至少明确设置：

```cmake
cmake_minimum_required(VERSION 3.16)

project(IndustrialGateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

## 6. 当前环境快照

本文件根据当前开发机在 2026-11-04 的命令输出整理。

### 已确认

- Ubuntu 22.04.5 LTS
- GCC / G++ 11.4.0
- CMake 3.22.1
- pkg-config 0.29.2-1ubuntu3
- SQLite 3.37.2-2ubuntu0.8
  - `sqlite3`
  - `libsqlite3-dev`
  - `libsqlite3-0`

### 已执行安装，待实际版本验证

- `libmodbus-dev=3.1.6-2`
- `libmodbus5=3.1.6-2`
- `libmosquitto-dev=2.0.11-1ubuntu1.2`
- `mosquitto-clients=2.0.11-1ubuntu1.2`
- `libmosquitto1`（实际版本待确认）

### 后续维护要求

每次增加或升级第三方依赖时：

1. 先查询软件源可用版本。
2. 确认安装版本。
3. 验证头文件、动态库和链接参数。
4. 完成项目编译和功能测试。
5. 更新本文件中的版本记录。
