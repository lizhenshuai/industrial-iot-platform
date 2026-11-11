"""通过已安装的 Mosquitto CLI 收发；不添加 Python MQTT 依赖。"""

import json
import subprocess
import threading
import time
import uuid


class MqttObserver:
    def __init__(self, owner, pub_binary, sub_binary, port, prefix):
        self.owner, self.pub_binary = owner, pub_binary
        self.port, self.prefix = port, prefix
        self.lock = threading.Lock()
        self.messages, self.errors = [], []
        self.mid_payloads = {}
        self.sequence = 0
        # 固定观察者 ID + 持久会话。Broker 重启后，订阅者重连前产生的 QoS1
        # 消息由 Broker 暂存，避免观察者自己丢消息造成补传测试误报。
        command = [sub_binary, "-h", "127.0.0.1", "-p", str(port), "-V", "mqttv311",
                   "-i", prefix.split("/")[-1] + "-observer", "-c", "-q", "1",
                   "-t", prefix + "/#", "-F", "%t\t%p"]
        self.process = owner.spawn("mqtt-observer", command, pipe=True)
        self.reader = threading.Thread(target=self._read, name="mqtt-observer-reader", daemon=True)
        self.reader.start()

    def _read(self):
        try:
            with (self.owner.path / "mqtt-messages.jsonl").open("w", buffering=1) as output:
                for line in self.process.popen.stdout:
                    if not line.strip():
                        continue
                    topic, payload = line.rstrip("\n").split("\t", 1)
                    payload = json.loads(payload)
                    row = {"monotonic": time.monotonic(), "wall_ms": time.time_ns() // 1000000,
                           "topic": topic, "payload": payload}
                    with self.lock:
                        if topic == self.prefix + "/telemetry":
                            if not isinstance(payload, dict) or not isinstance(payload.get("message_id"), str):
                                raise ValueError("遥测缺少合法 message_id")
                            mid = payload["message_id"]
                            previous = self.mid_payloads.setdefault(mid, payload)
                            if previous != payload:
                                raise ValueError(f"message_id {mid} 被用于不同内容")
                        self.messages.append(row)
                    output.write(json.dumps(row, ensure_ascii=False) + "\n")
        except Exception as error:
            with self.lock:
                self.errors.append(str(error))

    def check(self):
        with self.lock:
            if self.errors:
                raise AssertionError("MQTT 消息收集失败: " + "; ".join(self.errors))

    def snapshot(self):
        self.check()
        with self.lock:
            return list(self.messages)

    def publish(self, topic, payload):
        self.sequence += 1
        file = self.owner.path / f"publish-{self.sequence:03d}.json"
        file.write_text(json.dumps(payload, ensure_ascii=False))
        # 明确不使用 retained；重启恢复阶段只能从 SQLite 取配置。
        command = [self.pub_binary, "-h", "127.0.0.1", "-p", str(self.port),
                   "-V", "mqttv311", "-q", "1", "-t", topic, "-f", str(file)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError(f"MQTT 发布失败: {result.stderr.strip()}")

    def ready(self):
        marker = uuid.uuid4().hex
        topic = self.prefix + "/e2e-ready"
        next_publish = 0.0

        def received():
            nonlocal next_publish
            if any(row["topic"] == topic and row["payload"] == marker for row in self.snapshot()):
                return True
            if time.monotonic() >= next_publish:
                self.publish(topic, marker)
                next_publish = time.monotonic() + 1
            return False

        self.owner.wait(received, "MQTT 观察者订阅就绪")

    def close(self):
        self.process.stop()
        self.reader.join(timeout=5)
        if self.reader.is_alive():
            raise RuntimeError("MQTT 收集线程未退出")
        self.check()
