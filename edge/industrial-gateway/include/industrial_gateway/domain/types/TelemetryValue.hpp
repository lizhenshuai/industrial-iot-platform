
#pragma once

#include <cstdint>
#include <string>
#include <variant>


namespace industrial_gateway::domain::types
{

// ==================================================
// 统一采集数据值类型
// ==================================================
//
// 用于表示不同工业协议采集到的基本数据类型。
//
// bool        : 布尔类型
// int64_t     : 有符号整数
// uint64_t    : 无符号整数
// double      : 浮点数
// string      : 字符串
//
// 具体协议的数据类型在进入领域层时，
// 统一转换为上述类型。
// using就是起别名，方便后面好使用 using MyInt = int;，右边的这个容器可以保存为多个类型，安全的保存
using TelemetryValue = std::variant<
    bool,
    std::int64_t,
    std::uint64_t,
    double,
    std::string
>;

} // namespace industrial_gateway::domain::types
