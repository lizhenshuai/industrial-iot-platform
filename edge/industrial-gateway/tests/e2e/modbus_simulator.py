#!/usr/bin/env python3
"""测试专用多从站模拟器；固定数据和请求日志，不复用网关驱动实现。"""

import argparse
import asyncio
import json
import signal
import time
from pathlib import Path

from pymodbus import FramerType
from pymodbus.server import ModbusSerialServer, ModbusTcpServer
from pymodbus.simulator import DataType, SimData, SimDevice


async def serve(args):
    fixture = json.loads(args.fixture.read_text())
    devices = []
    for item in fixture["devices"]:
        if item["protocol"] != args.protocol:
            continue
        words = [0] * 64
        for address, value in item["registers"].items():
            words[int(address)] = value
        # 用分离的数据区，错误的功能码不会碰巧读到正确值。
        valid = [SimData(address=0, values=words, datatype=DataType.REGISTERS)]
        invalid = [SimData(address=1000, values=0, datatype=DataType.REGISTERS)]
        bits = lambda: [SimData(address=1000, values=False, datatype=DataType.BITS)]
        holding, inputs = (valid, invalid) if item["register_type"] == "holding" else (invalid, valid)
        devices.append(SimDevice(id=item["unit_id"], simdata=(bits(), bits(), holding, inputs)))

    with args.requests.open("w", buffering=1) as output:
        def trace(sending, pdu):
            if not sending:
                output.write(json.dumps({
                    "monotonic": time.monotonic(), "wall_ms": time.time_ns() // 1000000,
                    "protocol": args.protocol, "unit_id": pdu.dev_id,
                    "function": pdu.function_code, "address": pdu.address, "count": pdu.count,
                }) + "\n")
            return pdu

        if args.protocol == "tcp":
            server = ModbusTcpServer(devices, address=("127.0.0.1", args.port), trace_pdu=trace)
        else:
            server = ModbusSerialServer(devices, port=args.serial, framer=FramerType.RTU,
                                       baudrate=9600, bytesize=8, parity="N", stopbits=1,
                                       trace_pdu=trace)
        stopped = asyncio.Event()
        for sig in (signal.SIGTERM, signal.SIGINT):
            asyncio.get_running_loop().add_signal_handler(sig, stopped.set)
        try:
            # 返回表示监听已经建立，控制脚本不用猜测初始化需要几秒。
            await server.serve_forever(background=True)
            args.ready.write_text("ready\n")
            await stopped.wait()
        finally:
            await server.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", choices=["tcp", "rtu"], required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--ready", type=Path, required=True)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--serial", default="")
    asyncio.run(serve(parser.parse_args()))


if __name__ == "__main__":
    main()
