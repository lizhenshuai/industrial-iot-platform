
import asyncio
import random

from pymodbus.simulator import DataType, SimData, SimDevice
from pymodbus.server import ModbusTcpServer


# PLC 设备配置
DEVICE_ID = 1
HOST = "0.0.0.0"
PORT = 31502
REGISTER_COUNT = 100


async def main():

    # 1. 创建 PLC 保持寄存器
    holding_registers = SimData(
        address=0,
        count=REGISTER_COUNT,
        values=0,
        datatype=DataType.REGISTERS
    )

    # 2. 创建模拟 PLC 设备
    device = SimDevice(
        id=DEVICE_ID,
        simdata=holding_registers
    )

    # 3. 创建 Modbus TCP 服务端
    server = ModbusTcpServer(
        context=device,
        address=(HOST, PORT)
    )

    # 4. 定义设备数据更新任务
    async def update_device_data():

        temperature = 356
        re_count = 0

        while True:

            re_count += 1

            # 模拟电机数据变化
            speed = random.randint(1450, 1550)

            temperature += random.choice([-1, 0, 1])
            temperature = max(340, min(380, temperature))

            current = random.randint(110, 135)

            # 用递增值模拟寄存器变化
            running_status = re_count % 65536

            # 更新前四个保持寄存器
            values = [
                speed,
                temperature,
                current,
                running_status
            ]

            await server.async_setValues(
                device_id=DEVICE_ID,
                func_code=3,
                address=0,
                values=values
            )

            # 模拟写入一个 32 位数据
            runtime = 10000

            high = (runtime >> 16) & 0xFFFF
            low = runtime & 0xFFFF

            await server.async_setValues(
                device_id=DEVICE_ID,
                func_code=3,
                address=4,
                values=[high, low]
            )

            print(
                f"转速:{speed} rpm | "
                f"温度:{temperature / 10:.1f} ℃ | "
                f"电流:{current / 10:.1f} A | "
                f"运行状态:{running_status} | "
                f"运行时长:{runtime}"
            )

            # 每秒更新一次
            await asyncio.sleep(1)

    # 5. 创建后台数据更新任务
    update_task = asyncio.create_task(update_device_data())

    try:
        print(f"Modbus TCP 服务启动: {HOST}:{PORT}")

        # 6. 启动服务端，持续监听客户端连接
        await server.serve_forever()

    finally:
        # 服务退出时，取消后台更新任务
        update_task.cancel()

        try:
            await update_task
        except asyncio.CancelledError:
            pass


if __name__ == "__main__":
    asyncio.run(main())
