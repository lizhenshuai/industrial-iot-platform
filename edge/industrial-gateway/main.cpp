#include <iostream>  
#include <cstdint>
#include <thread>
#include <chrono>
#include <cerrno>
#include <modbus/modbus.h>

int main()
{
    // 1.配置modbus服务端的ip和端口配置
    const char* SERVER_IP = "192.168.0.83";
    const int SERVER_PORT = 31502;
    const int DEVICE_ID = 1;
    // 2.创建modbus的客户端，来连接服务端
    auto* ctx = modbus_new_tcp(
        SERVER_IP,
        SERVER_PORT
    );
    if(ctx == nullptr)
    {
        std::cerr << "创建modbus客户端失败" << std::endl;
        return -1;
    }
    // 设置设备id
    modbus_set_slave(ctx, DEVICE_ID);
    // 设置响应超时时间，第一个是秒，第二个是微秒，单次发送采集请求，超过指定时间超时
    modbus_set_response_timeout(ctx, 1, 0);
    // 连接modbus客户端
    if(modbus_connect(ctx) == -1)
    {
        std::cerr << "连接失败：" << modbus_strerror(errno) << std::endl;
        modbus_free(ctx);
        return -1;
    }
    std::cout << "Modbus TCP 连接成功" << std::endl;
    // 循环采集设备
    while(true)
    {
        // 保存读取到的寄存器
        uint16_t registers[6] = {};
        // 从地址0开始，读取6个保持寄存器，把读取到的数据都保存到数组中
        int rc = modbus_read_registers(
            ctx,
            0,
            6,
            registers
        );
        if(rc == -1)
        {
            std::cerr << "读取失败：" << modbus_strerror(errno) << std::endl;
            // 退出本次循环，但是要接着采集
            break;
        }
        if(rc != 6)
        {
            std::cerr << "读取寄存器数量异常" << std::endl;
            break;
        }
        // 解析寄存器数据
        // 转速
        int speed = registers[0];
        // 温度
        double temperature = registers[1] / 10.0;
        // 电流
        double current = registers[2] / 10.0;
        // 运行状态
        int running_status = registers[3];
        // 6. 组合两个 16 位寄存器，得到 32 位数据
        uint32_t runtime =
            (static_cast<uint32_t>(registers[4]) << 16)
            | static_cast<uint32_t>(registers[5]);
                // 7. 输出采集结果
        std::cout
            << "转速:" << speed << " rpm | "
            << "温度:" << temperature << " ℃ | "
            << "电流:" << current << " A | "
            << "运行状态:" << running_status << " | "
            << "运行时长:" << runtime
            << std::endl;
                // 每秒采集一次
        std::this_thread::sleep_for(
            std::chrono::microseconds(100)
        );
    }
    // 8. 释放资源
    modbus_close(ctx);
    modbus_free(ctx);
    return 0;
}