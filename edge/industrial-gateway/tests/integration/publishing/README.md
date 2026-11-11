# 遥测上报链路集成测试

测试使用真实的 `BoundedDataQueue`、`StorageWorker`、SQLite 文件、
`MqttPublisherWorker`、`MosquittoMqttClient` 和外部 MQTT Broker。
另一个 MQTT 客户端负责订阅，收到 SUBACK 后才开始发布。

## 覆盖内容

| 用例 | 验证 |
|---|---|
| `MultiDevicePacketRoundTrip` | 多设备、多点位、失败设备空数组；一包一行；SQLite JSON 与订阅 payload 逐字节一致；Topic、QoS 1、非 retained；确认后数据库清空 |
| `ConcurrentIngestionAndPublishingAcrossBatches` | 写入与发布线程并行运行；205 包跨越 100 包批次边界；正常连接下按序完整收到；最终无积压 |
| `ClientDisconnectBuffersAndReconnectDrains` | 只断开测试发布客户端；离线消息保存到 SQLite；恢复客户端连接后 Worker 自动补传，包内容不变 |
| `RecreatedGatewayPublishesPersistedBacklog` | 销毁并重新创建网关侧对象，读取同一 SQLite 文件，发送上次未发送的包 |

每个用例使用随机 `integration-...` 网关 ID、独立发布/订阅客户端 ID 和临时数据库。
不会修改项目数据库、配置文件，也不会停止共享 Broker。测试完成后删除自己的临时数据库。
默认不运行这些需要真实 Broker 的测试。它们会发布合成测试数据到
`v1/gateways/integration-.../telemetry`，应连接允许这类测试 Topic 的开发 Broker。

断线用例通过停止/重新启动发布客户端控制离线状态，不模拟实际网络断电；
重建用例验证对象重启后的磁盘积压恢复，不模拟进程强杀或断电。
ACK 超时、ACK 丢失/迟到、数据库删除失败属于故障注入测试，分别由现有
`MqttPublisherWorkerTest` 和 `MosquittoMqttClientTest` 覆盖。
真实 Broker 通常立即 ACK，不能通过“订阅端没收到”来判定发布端 ACK 超时。
QoS 1 允许故障恢复时重复投递，平台仍应按 `message_id` 去重；本文件的
正常网络用例检查无故障时的包数，不宣称提供 exactly-once 保证。

## 构建和运行

在项目根目录执行：

```bash
cmake -S . -B build
cmake --build build --target telemetry_pipeline_tests -j 4

# 默认读取 config/gateway.yaml 的 mqtt.enabled/host/port/keep_alive。
# 自动建立独立订阅连接，打印订阅到的第一包完整 JSON 和收包数量。
timeout 120s ./build/telemetry_pipeline_tests \
  --gtest_also_run_disabled_tests
```

仅查看单包完整链路：

```bash
./build/telemetry_pipeline_tests --gtest_also_run_disabled_tests \
  --gtest_filter=TelemetryPipelineTest.DISABLED_MultiDevicePacketRoundTrip
```

如需指定其他 Broker，创建单独的测试配置文件，保持应用配置不变：

```yaml
mqtt:
  enabled: true
  host: "127.0.0.1"
  port: 1883
  keep_alive: 60
```

```bash
MQTT_TEST_CONFIG=/tmp/gateway-mqtt-test.yaml \
  ./build/telemetry_pipeline_tests --gtest_also_run_disabled_tests
```

如果想在终端工具中另行看到消息，可以在专用开发 Broker 上先运行下面命令，
再执行测试。自动订阅客户端不依赖此工具：

```bash
mosquitto_sub -h <Broker地址> -p <端口> -q 1 \
  -t 'v1/gateways/+/telemetry' -v
```

该通配订阅会同时看到 Broker 上其他网关的遥测；通过测试输出的
`Subscribed topic` 和 `integration-` 网关 ID 识别本次测试消息。
测试发布不使用 retain，必须提前订阅才能看到实时消息。
