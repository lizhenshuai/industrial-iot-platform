"""对外部可观察结果做断言；预期数值来自独立的 fixture，不调用生产解码代码。"""

import copy
import json
import math
import statistics
from collections import Counter, defaultdict


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def points_for(fixture, device, updated):
    if device["protocol"] == "opcua":
        points = copy.deepcopy(device["points"])
        if updated:
            points = [p for p in points if p["property"] != "removed_value"]
            points.append(copy.deepcopy(device["added_point"]))
            for point in points:
                point["interval_ms"] = 5000
        return points
    points = copy.deepcopy(fixture["points"])
    if updated:
        points = [p for p in points if p["property"] != "signed_value"]
        points.append(copy.deepcopy(fixture["added_point"]))
        for point in points:
            point["interval_ms"] = 5000
    for point in points:
        point["register_type"] = device["register_type"]
    return points


def groups_for(fixture, protocols, updated):
    groups = defaultdict(dict)
    for device in fixture["devices"]:
        if device["protocol"] not in protocols:
            continue
        for point in points_for(fixture, device, updated):
            group = groups[(device["protocol"], point["interval_ms"])]
            group.setdefault(device["device_id"], {})[point["property"]] = device["expected"][point["property"]]
    return groups


def read_requests(path):
    if not path.exists():
        return []
    text = path.read_text()
    # 写入者可能正在追加一行，只读取已经完成的行，完整坏行仍然报错。
    return [json.loads(line) for line in text.splitlines(keepends=True) if line.endswith("\n")]


def check_requests(fixture, protocols, updated, paths, start, end):
    expected = {}
    widths = {"uint16": 1, "int16": 1, "float32": 2, "uint32": 2}
    for device in fixture["devices"]:
        if device["protocol"] not in protocols or device["protocol"] == "opcua":
            continue
        by_interval = defaultdict(list)
        for point in points_for(fixture, device, updated):
            by_interval[point["interval_ms"]].append(point)
        for interval, points in by_interval.items():
            begin = min(p["address"] for p in points)
            finish = max(p["address"] + widths[p["data_type"]] for p in points)
            key = (device["protocol"], device["unit_id"],
                   3 if device["register_type"] == "holding" else 4, begin, finish - begin)
            expected[key] = interval / 1000
    times = defaultdict(list)
    for protocol in protocols:
        if protocol == "opcua":
            continue
        for row in read_requests(paths[protocol]):
            if start <= row["monotonic"] <= end:
                key = (protocol, row["unit_id"], row["function"], row["address"], row["count"])
                require(key in expected, f"意外请求（频率组混合、拆包或旧配置仍在使用）: {key}")
                times[key].append(row["monotonic"])
    report = []
    for key, period in expected.items():
        samples = times[key]
        minimum = max(3, math.floor((end - start) / period) - 2)
        maximum = math.ceil((end - start) / period) + 2
        require(minimum <= len(samples) <= maximum,
                f"请求数量错误 {key}: {len(samples)}，期望 {minimum}..{maximum}")
        gaps = [b - a for a, b in zip(samples, samples[1:])]
        median = statistics.median(gaps)
        tolerance = max(0.35, period * 0.2)
        require(abs(median - period) <= tolerance, f"采集周期错误 {key}: 中位间隔 {median:.3f}s，目标 {period}s")
        require(min(gaps) >= period * 0.4, f"发现重复/过密请求 {key}: {min(gaps):.3f}s")
        require(max(gaps) <= period * 2 + tolerance, f"采集停顿过长 {key}: {max(gaps):.3f}s")
        report.append({"request": list(key), "count": len(samples), "median_interval_s": median})
    if "opcua" in protocols:
        report.extend(check_opcua_reads(fixture, updated, paths["opcua"], start, end))
    return report


def check_opcua_reads(fixture, updated, path, start, end):
    """按节点的数据源回调检查实际读取周期，不将回调数冒充 Read 服务请求数。"""
    expected = {}
    for device in fixture["devices"]:
        if device["protocol"] != "opcua":
            continue
        for point in points_for(fixture, device, updated):
            expected[(device["device_id"], point["node_id"])] = point["interval_ms"] / 1000
    times = defaultdict(list)
    for row in read_requests(path):
        if start <= row["monotonic"] <= end:
            require(row["protocol"] == "opcua" and row["kind"] == "node_read", "OPC UA 读取日志结构不符")
            key = (row["device_id"], row["node_id"])
            require(key in expected, f"OPC UA 仍在读已移除节点或意外节点: {key}")
            times[key].append(row["monotonic"])
    report = []
    for key, period in expected.items():
        samples = times[key]
        minimum = max(3, math.floor((end - start) / period) - 2)
        maximum = math.ceil((end - start) / period) + 2
        require(minimum <= len(samples) <= maximum,
                f"OPC UA 节点读取数量错误 {key}: {len(samples)}，期望 {minimum}..{maximum}")
        gaps = [b - a for a, b in zip(samples, samples[1:])]
        median = statistics.median(gaps)
        tolerance = max(0.35, period * 0.2)
        require(abs(median - period) <= tolerance,
                f"OPC UA 周期错误 {key}: {median:.3f}s，目标 {period}s")
        require(min(gaps) >= period * 0.4, f"OPC UA 节点重复/过密读取: {key}")
        require(max(gaps) <= period * 2 + tolerance, f"OPC UA 节点采集中断过长: {key}")
        report.append({"kind": "node_read", "protocol": "opcua", "device_id": key[0],
                       "node_id": key[1], "count": len(samples), "median_interval_s": median})
    return report


def check_value(value, expected, label):
    # bool 是 Python int 的子类，必须单独判断；64 位整数用精确比较，
    # 不能先转 double 或使用 isclose，否则会漏掉 JSON 整数精度损失。
    if type(expected) in (bool, str, int):
        require(type(value) is type(expected) and value == expected,
                f"值或类型错误 {label}: {value!r} != {expected!r}")
    else:
        require(type(expected) is float and type(value) in (int, float) and
                math.isfinite(value) and math.isclose(value, expected, rel_tol=1e-9, abs_tol=1e-6),
                f"浮点值错误 {label}: {value!r} != {expected!r}")


def packet_group(packet, groups):
    require(isinstance(packet, dict), "遥测必须是 JSON 对象")
    require(set(packet) == {"message_id", "timestamp_ms", "devices"}, "遥测顶层结构不符")
    require(isinstance(packet["message_id"], str) and packet["message_id"], "message_id 缺失")
    require(type(packet["timestamp_ms"]) is int and packet["timestamp_ms"] > 0, "非法毫秒时间戳")
    require(isinstance(packet["devices"], list) and packet["devices"], "devices 必须是非空数组")
    actual = {}
    for device in packet["devices"]:
        require(set(device) == {"device_id", "status", "records"}, "设备字段不符")
        did = device["device_id"]
        require(type(did) is int and did not in actual, "设备 ID 类型错误或重复")
        require(device["status"] == "Success", f"正常模拟设备采集失败: {device}")
        require(isinstance(device["records"], list), "records 必须是数组")
        values = {}
        for record in device["records"]:
            require(set(record) == {"property", "value"}, "点位结构不符")
            name = record["property"]
            require(isinstance(name, str) and name not in values, "属性类型错误或重复")
            require(type(record["value"]) in (bool, int, float, str), f"属性 {name} 不是支持的标量")
            values[name] = record["value"]
        actual[did] = values
    candidates = [(key, target) for key, target in groups.items()
                  if actual.keys() == target.keys()
                  and all(actual[did].keys() == target[did].keys() for did in actual)]
    require(len(candidates) == 1, f"设备/属性组错误，可能混合协议或频率: {actual}")
    key, target = candidates[0]
    for did, values in actual.items():
        for name, value in values.items():
            check_value(value, target[did][name], f"device={did} {name}")
    return key


def check_packets(messages, topic, groups, wall_start, wall_end, duration):
    seen, counts = {}, Counter()
    for message in messages:
        if message["topic"] != topic:
            continue
        packet = message["payload"]
        require(isinstance(packet, dict), "遥测必须是 JSON 对象")
        stamp = packet.get("timestamp_ms")
        require(type(stamp) is int, "遥测时间戳不是整数")
        # 把旧配置已经入库、稍后才发送的数据与本观察窗口分开。
        if not wall_start <= stamp <= wall_end:
            continue
        key = packet_group(packet, groups)
        mid = packet["message_id"]
        if mid in seen:
            require(seen[mid] == packet, f"同一 message_id 对应不同内容: {mid}")
            continue
        seen[mid] = packet
        counts[key] += 1
    for key in groups:
        minimum = max(3, math.floor(duration / (key[1] / 1000)) - 2)
        require(counts[key] >= minimum, f"遥测包不足 {key}: {counts[key]} < {minimum}")
    return {f"{protocol}/{interval}ms": counts[(protocol, interval)] for protocol, interval in groups}
