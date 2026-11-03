# DEPENDENCIES

本文档记录 `industrial-iot-platform` 项目的开发环境与第三方依赖版本。
目标是让项目在相同环境下可重复构建，并降低依赖升级带来的不确定性。

## 1. 开发环境

  组件               固定/记录版本        说明
  ------------------ -------------------- ------------------------------------
  Operating System   Ubuntu 22.04.5 LTS   当前开发系统
  Compiler           GCC / G++ 11.4.0     `g++ --version` 检测结果：`11.4.0`
  C++ Standard       C++17                项目统一使用 C++17
  CMake              3.22.1               `cmake --version` 检测结果
  pkg-config         0.29.2-1ubuntu3      当前已安装版本

## 2. 第三方依赖

  --------------------------------------------------------------------------------------------
  依赖              软件包            锁定版本          状态
  ----------------- ----------------- ----------------- --------------------------------------
  libmodbus         `libmodbus-dev`   `3.1.6-2`         软件源候选版本已确认，安装状态待验证
  开发文件                                              

  libmodbus         `libmodbus5`      `3.1.6-2`         软件源候选版本已确认，安装状态待验证
  运行时库                                              
  --------------------------------------------------------------------------------------------

> 注：`3.1.6-2` 来自 Ubuntu 22.04（jammy）当前软件源的
> `apt-cache policy`
> 查询结果。执行安装并验证后，再将状态更新为"已安装并验证"。

## 3. 安装命令

在 Ubuntu 22.04 环境中，使用指定版本安装 libmodbus：

``` bash
sudo apt install -y \
    libmodbus-dev=3.1.6-2 \
    libmodbus5=3.1.6-2
```

如果软件源中不再提供该版本，先检查软件源配置和可用版本，不要直接改成未验证的版本。

## 4. 版本验证命令

``` bash
# 操作系统
lsb_release -ds

# 编译器
g++ --version

# CMake
cmake --version

# pkg-config
pkg-config --version

# libmodbus 软件包版本
dpkg-query -W -f='${binary:Package}=${Version}\n' \
    libmodbus-dev libmodbus5

# libmodbus 库版本
pkg-config --modversion libmodbus

# libmodbus 编译与链接参数
pkg-config --cflags --libs libmodbus
```

## 5. 项目构建约定

-   项目统一使用 C++17。
-   使用 CMake 管理构建。
-   第三方依赖应记录准确版本，不依赖系统中未记录的"最新版"。
-   修改依赖版本前，先在开发环境验证编译、运行和相关测试。
-   依赖升级后，及时更新本文档。
-   生产部署应使用经过验证的依赖版本；不要在生产设备上未经测试直接升级系统依赖。

CMake 中至少明确设置：

``` cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

## 6. 当前环境快照

本文件根据当前开发机在 2026-11-03 的命令输出整理。

已确认： - Ubuntu 22.04.5 LTS - G++ 11.4.0 - CMake 3.22.1 - pkg-config
0.29.2-1ubuntu3 - libmodbus-dev 与 libmodbus5 的软件源候选版本均为
3.1.6-2

待完成： - 安装 `libmodbus-dev=3.1.6-2` 和 `libmodbus5=3.1.6-2` -
执行版本验证命令，并确认实际安装版本
