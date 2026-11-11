#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>


namespace industrial_gateway::drivers::modbus_rtu
{

// ==================================================
// 寄存器读取及解码类型
// ==================================================

// 第一版采集保持寄存器和输入寄存器，不包含写操作。
enum class ModbusRegisterType
{
    Holding,
    Input
};

// 一个点位表示一个数值，占用的寄存器数量由数据类型推导：
// Int16 / UInt16：1；Int32 / UInt32 / Float32：2；
// Int64 / UInt64 / Float64：4。
// 不额外配置 quantity，避免类型与寄存器数量互相矛盾。
enum class ModbusDataType
{
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64
};

// 数值编码中，一个寄存器内部两个字节的有效顺序。
// 这是设备数值解码配置，不改变 Modbus 报文传输格式。
enum class ModbusByteOrder
{
    HighByteFirst,
    LowByteFirst
};

// 多寄存器数值按照地址升序读取后，各寄存器的有效顺序。
// 单寄存器类型忽略此设置；第一版只支持以下两种排列。
enum class ModbusWordOrder
{
    HighWordFirst,
    LowWordFirst
};


// ==================================================
// 点位配置
// ==================================================

struct ModbusRtuPointConfig
{
    // 逻辑点位名称，对应任务 properties 和遥测记录 property。
    // 必须非空，同一设备内不能重复。
    std::string property;

    ModbusRegisterType register_type{ModbusRegisterType::Holding};

    // 协议中的零基起始地址，不直接填写设备手册中的 4xxxx 等引用编号。
    // 完整点位占用范围也必须落在有效地址范围内。
    std::uint16_t address{0};

    ModbusDataType data_type{ModbusDataType::UInt16};
    ModbusByteOrder byte_order{ModbusByteOrder::HighByteFirst};
    ModbusWordOrder word_order{ModbusWordOrder::HighWordFirst};

    // 采集周期，必须大于 0。
    // 同连接内按周期构建 CollectionTask，不同周期分别执行、分别组包。
    std::chrono::milliseconds interval{1000};

    // 工程值 = 解码值 * scale + offset；两项必须为有限数值。
    // 默认不缩放：整数保留为 int64_t / uint64_t，浮点数转换为 double。
    // 启用非默认缩放时输出 double，大整数转 double 可能损失精度。
    // 解码值或最终工程值非有限时，按设备本次采集失败处理。
    double scale{1.0};
    double offset{0.0};
};


// ==================================================
// 设备配置
// ==================================================

struct ModbusRtuDeviceConfig
{
    // 平台设备 ID，对应任务和遥测包中的 device_id。
    // 同一连接配置中不能重复，不等同于 Unit ID。
    std::int64_t device_id{0};

    // Modbus 请求中的设备地址，由协议配置解析器校验有效范围。
    int unit_id{1};

    // 设备采集点位，必须非空。
    // 同设备、同周期的点位必须属于同一种 register_type，
    // 并能通过一次连续范围读取覆盖，且不超过协议请求数量限制。
    // 不满足条件时应在配置校验阶段拒绝，第一版不自动拆分请求。
    // 如果点位之间存在间隙，中间地址也会被读取，其可读性需设备联调确认。
    std::vector<ModbusRtuPointConfig> points;
};


// ==================================================
// 一个本地串口总线的协议配置；同串口上的设备共用相同串口参数。
// ==================================================
//
// 仅保存配置，不创建连接、线程，也不执行解析或校验。
// connection_id 由公共 ConnectionDefinition 提供，不在此重复保存。
// 协议解析器负责校验字段以及“一设备一频率一次请求”的组合约束。
// 解析数值时须先检查范围，再转换为窄整数类型，避免截断。
//
// ==================================================

struct ModbusRtuConfig
{
    // Linux 串口绝对路径。建议使用 /dev/serial/by-id 下的稳定名称。
    // 配置阶段允许设备尚未插入，实际打开失败由 Worker 负责重试。
    std::string device;
    int baud_rate{9600};
    char parity{'N'}; // N / E / O
    int data_bits{8}; // RTU 使用 8 位数据，不沿用 ASCII 模式的 7 位配置。
    int stop_bits{1}; // 1 / 2，必须与设备实际参数一致。

    // default 不改变内核 RS485 模式，适用于自动方向控制的 USB 转换器。
    // rs485 / rs232 显式切换内核串口模式，需内核驱动支持 RS485 ioctl。
    std::string serial_mode{"default"};

    // 单次读取请求等待完整响应的超时，1..60000ms。
    // 多设备任务的总执行时间可能大于此值。
    std::chrono::milliseconds response_timeout{1000};

    // 超时/CRC/残帧后先隔离这么长时间，期间排空迟到数据，再等待帧间静默。
    // 1..60000ms；不是无限重试，不保证隔离超过该窗口才到达的异常响应。
    std::chrono::milliseconds recovery_delay{1000};

    // 请求之间额外等待，0..1000ms；实际还会保证至少 3.5 字符的帧间隔。
    std::chrono::milliseconds inter_request_delay{0};

    // 此端点下的设备集合，必须非空。
    std::vector<ModbusRtuDeviceConfig> devices;
};

} // namespace industrial_gateway::drivers::modbus_rtu
