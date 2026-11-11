
import asyncio
import random

from pymodbus import FramerType
from pymodbus.simulator import DataType, SimData, SimDevice
from pymodbus.server import ModbusSerialServer


# =====================================
# 1. PLC 配置
# =====================================

DEVICE_ID = 1

SERIAL_PORT = "/tmp/ttyV0"

BAUDRATE = 9600

REGISTER_COUNT = 100


# =====================================
# 2. 创建模拟 PLC
# =====================================

def create_device() -> SimDevice:

    # 创建 100 个保持寄存器
    holding_registers = SimData(
        address=0,
        count=REGISTER_COUNT,
        values=0,
        datatype=DataType.REGISTERS
    )

    # 创建模拟设备
    device = SimDevice(
        id=DEVICE_ID,
        simdata=holding_registers
    )

    return device


# =====================================
# 3. 模拟 PLC 内部数据变化
# =====================================

async def update_device_data(server):

    temperature = 356

    while True:

        # 模拟电机数据变化
        speed = random.randint(1450, 1550)

        temperature += random.choice([-1, 0, 1])
        temperature = max(340, min(380, temperature))

        current = random.randint(110, 135)

        running_status = 1

        # 更新 PLC 保持寄存器
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

        print(
            f"转速:{speed} rpm | "
            f"温度:{temperature / 10:.1f} ℃ | "
            f"电流:{current / 10:.1f} A | "
            f"运行状态:{running_status}"
        )

        await asyncio.sleep(1)


# =====================================
# 4. 启动 Modbus RTU 服务端
# =====================================

async def main():

    # 创建模拟设备
    device = create_device()

    # 创建 RTU 服务端
    server = ModbusSerialServer(
        context=device,
        port=SERIAL_PORT,
        framer=FramerType.RTU,
        baudrate=BAUDRATE,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=1
    )

    print("Modbus RTU 模拟 PLC 启动")
    print(f"串口: {SERIAL_PORT}")
    print(f"从站地址: {DEVICE_ID}")
    print(f"波特率: {BAUDRATE}")

    # 后台更新 PLC 数据
    update_task = asyncio.create_task(
        update_device_data(server)
    )

    try:
        # 持续等待主站请求
        await server.serve_forever()

    finally:
        update_task.cancel()

        await asyncio.gather(
            update_task,
            return_exceptions=True
        )

        await server.shutdown()


if __name__ == "__main__":
    asyncio.run(main())

