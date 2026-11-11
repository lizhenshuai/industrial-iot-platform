#!/usr/bin/env python3
"""真实进程全链路测试：TCP、RTU、双协议、OPC UA、三协议。显式运行才启动服务。"""

import argparse
import json
import math
import os
import pwd
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import traceback
import uuid
from pathlib import Path

from assertions import (check_packets, check_requests, groups_for, packet_group,
                        points_for, read_requests, require)
from mqtt_client import MqttObserver

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent.parent
SCENARIOS = {
    "tcp": ["tcp"],
    "rtu": ["rtu"],
    "both": ["tcp", "rtu"],
    "opcua": ["opcua"],
    "mixed": ["tcp", "rtu", "opcua"],
}


def selected_protocols(scenario):
    names = list(SCENARIOS) if scenario == "all" else [scenario]
    return {protocol for name in names for protocol in SCENARIOS[name]}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def free_port():
    # 仅绑定回环接口。释放后仍有竞争窗口，服务启动失败会明确报错而不会误判成功。
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Child:
    def __init__(self, name, command, path, pipe=False):
        self.name, self.expected_stop = name, False
        self.log_path = path / f"{name}.log"
        self.log = self.log_path.open("w", buffering=1)
        try:
            self.popen = subprocess.Popen(command, cwd=path, stdin=subprocess.DEVNULL,
                                          stdout=subprocess.PIPE if pipe else self.log,
                                          stderr=self.log, text=True, encoding="utf-8",
                                          errors="replace", start_new_session=True)
        except BaseException:
            self.log.close()
            raise

    def stop(self):
        if self.expected_stop:
            return
        self.expected_stop = True
        forced = False
        try:
            if self.popen.poll() is None:
                try:
                    os.killpg(self.popen.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    self.popen.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    forced = True
                    try:
                        os.killpg(self.popen.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    self.popen.wait(timeout=5)
        finally:
            self.log.close()
        if forced:
            raise RuntimeError(f"{self.name} 停止超时，已强制清理本次创建的进程组")


class Scenario:
    def __init__(self, name, args, root, fixture):
        self.name, self.args, self.fixture = name, args, fixture
        self.path = root / name
        self.path.mkdir()
        self.protocols = SCENARIOS[name]
        self.children, self.request_paths = [], {}
        self.observer = self.gateway = self.broker = None
        self.broker_round = self.gateway_round = 0
        self.mqtt_port = free_port()
        self.tcp_port = free_port()
        while self.tcp_port == self.mqtt_port:
            self.tcp_port = free_port()
        self.opcua_port = free_port()
        while self.opcua_port in (self.mqtt_port, self.tcp_port):
            self.opcua_port = free_port()
        self.gateway_id = f"e2e-{name}-{uuid.uuid4().hex[:12]}"
        self.prefix = f"v1/gateways/{self.gateway_id}"
        self.db = self.path / "gateway.db"
        self.summary = {"scenario": name, "gateway_id": self.gateway_id, "phases": []}

    def spawn(self, name, command, pipe=False):
        child = Child(name, [str(part) for part in command], self.path, pipe)
        self.children.append(child)
        return child

    def health(self):
        for child in self.children:
            if not child.expected_stop and child.popen.poll() is not None:
                raise RuntimeError(f"{child.name} 意外退出: {child.popen.returncode}，查看 {child.log_path}")
        if self.observer:
            self.observer.check()

    def wait(self, predicate, label, timeout=None):
        deadline = time.monotonic() + (timeout or self.args.timeout)
        while time.monotonic() < deadline:
            self.health()
            value = predicate()
            if value:
                return value
            time.sleep(0.1)
        raise TimeoutError(f"等待超时: {label}")

    def pause(self, seconds):
        end = time.monotonic() + seconds
        self.wait(lambda: time.monotonic() >= end, f"观察 {seconds}s", seconds + 5)

    def rows(self, sql):
        if not self.db.exists():
            return []
        # 观察者只读数据库，不能替生产代码建表或改变状态。
        with sqlite3.connect(self.db.as_uri() + "?mode=ro", uri=True, timeout=2) as db:
            return db.execute(sql).fetchall()

    def start_broker(self):
        self.broker_round += 1
        self.broker = self.spawn(f"broker-{self.broker_round}",
                                 [self.args.broker, "-c", self.path / "mosquitto.conf", "-v"])

        def listening():
            try:
                with socket.create_connection(("127.0.0.1", self.mqtt_port), timeout=0.2):
                    return True
            except OSError:
                return False

        self.wait(listening, "专用 Broker 监听")

    def start_gateway(self):
        self.gateway_round += 1
        self.gateway = self.spawn(f"gateway-{self.gateway_round}",
                                  [self.args.gateway, self.path / "gateway.yaml"])
        self.wait(lambda: "配置 Topic 订阅成功" in self.gateway.log_path.read_text(errors="replace"),
                  "网关配置订阅成功")

    def stop_gateway(self):
        self.gateway.stop()
        require(self.gateway.popen.returncode == 0,
                f"网关未正常退出: {self.gateway.popen.returncode}")

    def setup(self):
        print(f"[{self.name}] 启动隔离环境: {self.path}", flush=True)
        # 独立 Broker，持久化观察者会话，以便验证断线补传。绝不停止系统 Broker。
        broker_data = self.path / "broker-data"
        broker_data.mkdir()
        (self.path / "mosquitto.conf").write_text(
            f"user {pwd.getpwuid(os.geteuid()).pw_name}\n"
            f"listener {self.mqtt_port} 127.0.0.1\nallow_anonymous true\n"
            f"persistence true\npersistence_location {broker_data}/\nautosave_interval 1\n"
            "log_dest stdout\nlog_type all\nmax_queued_messages 10000\n")
        self.start_broker()
        self.observer = MqttObserver(self, self.args.mqtt_pub, self.args.mqtt_sub,
                                     self.mqtt_port, self.prefix)
        self.observer.ready()
        if "rtu" in self.protocols:
            self.spawn("socat", [self.args.socat, "-d", "-d",
                                  f"pty,raw,echo=0,link={self.path / 'gateway.tty'}",
                                  f"pty,raw,echo=0,link={self.path / 'simulator.tty'}"])
            self.wait(lambda: (self.path / "gateway.tty").exists() and
                      (self.path / "simulator.tty").exists(), "虚拟串口创建")
        for protocol in self.protocols:
            requests = self.path / f"{protocol}-requests.jsonl"
            ready = self.path / f"{protocol}.ready"
            self.request_paths[protocol] = requests
            if protocol == "opcua":
                command = [self.args.opcua_simulator, "--port", self.opcua_port,
                           "--fixture", HERE / "fixtures/opcua.json",
                           "--requests", requests, "--ready", ready]
            else:
                command = [self.args.sim_python, "-u", HERE / "modbus_simulator.py",
                           "--protocol", protocol, "--fixture", HERE / "fixtures/devices.json",
                           "--requests", requests, "--ready", ready,
                           "--port", self.tcp_port, "--serial", self.path / "simulator.tty"]
            self.spawn(f"{protocol}-simulator", command)
            self.wait(ready.exists, f"{protocol} 模拟器监听")
        # JSON 是 YAML 的子集；由标准库生成，避免再增加 PyYAML 依赖。
        write_json(self.path / "gateway.yaml", {
            "schema_version": 1, "gateway": {"id": self.gateway_id},
            "logging": {"level": "info", "console_enabled": True, "file_enabled": False},
            "mqtt": {"enabled": True, "host": "127.0.0.1", "port": self.mqtt_port,
                     "client_id": self.gateway_id, "keep_alive": 10},
            "storage": {"database_path": str(self.db), "busy_timeout_ms": 5000},
            "connections": [],
        })
        self.start_gateway()
        # 空数据库启动不应自行启动设备采集。
        self.pause(1)
        require(not any(read_requests(p) for p in self.request_paths.values()), "下发配置前已开始采集")

    def snapshot(self, updated):
        connections = []
        for protocol in self.protocols:
            if protocol == "opcua":
                connection = {"endpoint_url": f"opc.tcp://127.0.0.1:{self.opcua_port}",
                              "timeout_ms": 3000, "security_mode": "None", "authentication": "anonymous"}
            elif protocol == "tcp":
                connection = {"host": "127.0.0.1", "port": self.tcp_port,
                              "response_timeout_ms": 1000, "connect_timeout_ms": 1000}
            else:
                connection = {"device": str(self.path / "gateway.tty"), "baud_rate": 9600,
                              "parity": "N", "data_bits": 8, "stop_bits": 1,
                              "serial_mode": "default", "response_timeout_ms": 1000,
                              "recovery_delay_ms": 1000}
            devices = []
            for device in self.fixture["devices"]:
                if device["protocol"] != protocol:
                    continue
                configured = {"device_id": device["device_id"],
                              "points": points_for(self.fixture, device, updated)}
                if protocol != "opcua":
                    configured["unit_id"] = device["unit_id"]
                devices.append(configured)
            protocol_type = "opcua" if protocol == "opcua" else "modbus_" + protocol
            connections.append({"connection_id": protocol + "-bus", "protocol": protocol_type,
                                "config": {"connection": connection, "devices": devices}})
        return {"schema_version": 1, "request_id": uuid.uuid4().hex, "connections": connections}

    def config_row(self):
        rows = self.rows("SELECT id, updated_at_ms, config_json FROM gateway_collection_config")
        require(len(rows) == 1 and rows[0][0] == 1 and rows[0][1] > 0, "配置表不是单条有效记录")
        return rows[0]

    def apply(self, updated):
        message = self.snapshot(updated)
        write_json(self.path / ("config-updated.json" if updated else "config-initial.json"), message)
        self.observer.publish(self.prefix + "/config", message)

        def acknowledged():
            for row in self.observer.snapshot():
                if row["topic"] != self.prefix + "/config/ack":
                    continue
                ack = row["payload"]
                if ack.get("request_id") == message["request_id"]:
                    require(ack.get("status") == "Applied" and ack.get("error_message") == "",
                            f"配置未成功应用: {ack}")
                    return True
            return False

        self.wait(acknowledged, "配置 Applied ACK")
        require(json.loads(self.config_row()[2]) == message, "SQLite 保存内容与下发配置不同")

    def observe(self, label, updated):
        print(f"[{self.name}] {label}：观察 {self.args.window}s", flush=True)
        # 等待打开串口及首次恢复窗口；后续按采集时间划分阶段，不以 MQTT 到达时间划分。
        self.pause(2)
        start, wall_start = time.monotonic(), time.time_ns() // 1000000
        self.pause(self.args.window)
        end, wall_end = time.monotonic(), time.time_ns() // 1000000
        require(abs((wall_end - wall_start) / 1000 - (end - start)) < 1,
                "观察期间系统时钟跳变，不能可靠校验包时间戳")
        groups = groups_for(self.fixture, self.protocols, updated)
        requests = check_requests(self.fixture, self.protocols, updated, self.request_paths, start, end)
        # 采集完成后仍可能在排队、落库或等待发布，给流水线独立的排空等待。
        result, last_error = {}, []

        def delivered():
            try:
                result["packets"] = check_packets(self.observer.snapshot(), self.prefix + "/telemetry",
                                                 groups, wall_start, wall_end, end - start)
                return True
            except AssertionError as error:
                last_error[:] = [str(error)]
                return False

        try:
            self.wait(delivered, f"{label} 遥测送达")
        except TimeoutError as error:
            raise AssertionError(f"{error}: {last_error}") from error
        self.summary["phases"].append({"name": label, "requests": requests, **result})

    def restart(self):
        print(f"[{self.name}] 重启网关，禁止重新下发配置", flush=True)
        saved = self.config_row()
        self.stop_gateway()
        # Broker 从未接收 retained 配置，且使用全新 gateway_id，因此这里没有 MQTT 恢复来源。
        self.start_gateway()
        self.observe("SQLite 重启恢复", updated=True)
        require(self.config_row() == saved, "仅恢复配置却改写了配置记录/保存时间")

    def backlog(self):
        print(f"[{self.name}] Broker 断线、SQLite 积压及恢复补传", flush=True)
        self.broker.stop()
        require(self.broker.popen.returncode == 0, "测试 Broker 未正常保存会话并退出")
        offline_start = time.time_ns() // 1000000
        groups = groups_for(self.fixture, self.protocols, True)

        def accumulated():
            packets = [json.loads(row[0]) for row in self.rows("SELECT message FROM messages")]
            fresh = [p for p in packets if p["timestamp_ms"] >= offline_start]
            found = {packet_group(p, groups) for p in fresh}
            return fresh if found == set(groups) and len(fresh) >= 4 else None

        stored = self.wait(accumulated, "所有当前协议均产生离线持久化数据")
        write_json(self.path / "offline-packets.json", stored)
        expected = {p["message_id"]: p for p in stored}
        # Broker 正常退出保存了持久会话；观察者在重连前的数据可由该会话补齐。
        self.start_broker()
        self.observer.ready()

        def replayed():
            actual = {row["payload"]["message_id"]: row["payload"] for row in self.observer.snapshot()
                      if row["topic"] == self.prefix + "/telemetry"}
            for mid in expected.keys() & actual.keys():
                require(actual[mid] == expected[mid], f"补传改变了 message_id/时间戳/内容: {mid}")
            return expected.keys() <= actual.keys()

        self.wait(replayed, "离线消息完整补传", timeout=max(60, self.args.timeout))
        # dict_keys 支持集合运算符，但没有 intersection 方法；先转为 set。
        self.wait(lambda: not set(expected).intersection(
            json.loads(row[0])["message_id"] for row in self.rows("SELECT message FROM messages")),
            "已补传消息从 SQLite 删除")
        self.summary["phases"].append({"name": "断线补传", "verified_message_ids": sorted(expected)})

    def cleanup(self):
        errors = []
        # 先退出网关，保持 Broker/模拟器活着，避免清理本身制造通信故障。
        if self.gateway and not self.gateway.expected_stop:
            try:
                self.stop_gateway()
            except Exception as error:
                errors.append(str(error))
        if self.observer:
            try:
                self.observer.close()
            except Exception as error:
                errors.append(str(error))
        for child in reversed(self.children):
            try:
                child.stop()
            except Exception as error:
                errors.append(str(error))
        return errors


def executable(value):
    found = shutil.which(str(value), path=os.environ.get("PATH", "") + ":/usr/sbin:/sbin")
    if not found:
        raise ValueError(f"缺少可执行文件: {value}。请自行准备依赖；脚本不会安装或下载。")
    return str(Path(found).absolute())  # 不 resolve 虚拟环境 python 符号链接，保留其环境。


def preflight(args):
    require(sys.platform.startswith("linux"), "当前 E2E 仅支持 Linux")
    protocols = selected_protocols(args.scenario)
    for field in ("gateway", "broker", "mqtt_pub", "mqtt_sub"):
        setattr(args, field, executable(getattr(args, field)))
    if "opcua" in protocols:
        args.opcua_simulator = executable(args.opcua_simulator)
    if "rtu" in protocols:
        args.socat = executable(args.socat)
    if not protocols.intersection({"tcp", "rtu"}):
        return  # 单独 OPC UA 场景不需要 pymodbus、pyserial 或 socat。
    args.sim_python = executable(args.sim_python)
    # 只在用户运行测试时检查模拟器依赖，不自动创建环境、不调用 uv 同步或 pip。
    result = subprocess.run([args.sim_python, "-c",
                             "import importlib.metadata as m; import serial; "
                             "from pymodbus.simulator import SimData, SimDevice; "
                             "print(m.version('pymodbus'))"], capture_output=True, text=True, timeout=10)
    require(result.returncode == 0, "模拟器 Python 环境不可用: " + result.stderr)
    require(result.stdout.strip() == "3.16.1", "模拟器接口针对 pymodbus 3.16.1，请指定对应 --sim-python")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=["all", *SCENARIOS], default="all")
    parser.add_argument("--gateway", default=str(PROJECT / "build/industrial_gateway"))
    parser.add_argument("--sim-python", default=str(PROJECT.parent / "simulators/.venv/bin/python"))
    parser.add_argument("--opcua-simulator", default=str(PROJECT / "build-e2e-opcua/opcua_e2e_server"),
                        help="预先编译的 OPC UA 测试服务器，不会自动构建")
    parser.add_argument("--broker", default="mosquitto")
    parser.add_argument("--mqtt-pub", default="mosquitto_pub")
    parser.add_argument("--mqtt-sub", default="mosquitto_sub")
    parser.add_argument("--socat", default="socat")
    parser.add_argument("--output", type=Path, help="报告父目录；总是创建新的子目录，不覆盖旧报告")
    parser.add_argument("--window", type=float, default=22, help="每阶段观察秒数，至少 20")
    parser.add_argument("--timeout", type=float, default=30, help="条件等待超时秒数，至少 15")
    args = parser.parse_args()
    if not math.isfinite(args.window) or not 20 <= args.window <= 300 or \
            not math.isfinite(args.timeout) or not 15 <= args.timeout <= 300:
        parser.error("--window 必须在 20..300，--timeout 必须在 15..300")
    try:
        preflight(args)
    except Exception as error:
        print(f"环境检查失败: {error}", file=sys.stderr)
        return 2
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="gateway-e2e-", dir=args.output)).resolve()
    # socat 的选项用逗号分隔，拒绝会改变其参数含义的路径。
    if any(char in str(root) for char in (",", "\n", "\r")):
        print("报告路径不能包含逗号或换行", file=sys.stderr)
        return 2
    print(f"报告目录: {root}", flush=True)
    fixture = json.loads((HERE / "fixtures/devices.json").read_text())
    if "opcua" in selected_protocols(args.scenario):
        opcua_fixture = json.loads((HERE / "fixtures/opcua.json").read_text())
        fixture["devices"].extend(opcua_fixture["devices"])
    # 即使外部 CI 给控制脚本发送 SIGTERM，也走 finally 清理自己的子进程。
    def interrupted_by_signal(_signal, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted_by_signal)
    names = list(SCENARIOS) if args.scenario == "all" else [args.scenario]
    summaries, interrupted = [], False
    for name in names:
        scenario = Scenario(name, args, root, fixture)
        try:
            scenario.setup()
            scenario.apply(updated=False)
            scenario.observe("首次配置采集", updated=False)
            scenario.apply(updated=True)
            scenario.observe("频率及属性更新", updated=True)
            scenario.restart()
            if name in ("both", "opcua", "mixed"):
                scenario.backlog()
            scenario.health()
            scenario.summary["status"] = "PASS"
        except KeyboardInterrupt:
            interrupted = True
            scenario.summary.update(status="FAIL", error="用户中断")
        except Exception as error:
            scenario.summary.update(status="FAIL", error=str(error))
            (scenario.path / "failure.txt").write_text(traceback.format_exc())
        finally:
            errors = scenario.cleanup()
            if errors:
                scenario.summary.update(status="FAIL", cleanup_errors=errors)
            write_json(scenario.path / "result.json", scenario.summary)
            summaries.append(scenario.summary)
            write_json(root / "report.json", summaries)
        print(f"[{name}] {scenario.summary['status']} {scenario.summary.get('error', '')}", flush=True)
        if interrupted:
            break
    print(f"完整报告: {root / 'report.json'}", flush=True)
    return 130 if interrupted else (0 if all(s["status"] == "PASS" for s in summaries) else 1)


if __name__ == "__main__":
    raise SystemExit(main())
