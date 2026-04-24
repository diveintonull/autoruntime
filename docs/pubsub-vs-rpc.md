# Pub/Sub 与 Request/Response 的选择

## 结论先行

两者不是“异步和同步”的简单替代关系，而是在表达不同的数据关系：

| 问题 | Pub/Sub | Request/Response |
| --- | --- | --- |
| 关系 | 一个 producer 面向零到多个 consumer | 一个 caller 针对一个逻辑 service |
| 时间语义 | 数据持续产生，consumer 按自己的节奏处理 | caller 针对一次问题等待一次结果 |
| 解耦 | producer 不需要知道 consumer 是否存在 | caller 需要 service 在 deadline 内回应 |
| 失败结果 | 丢样、积压、QoS 不匹配、订阅者离线 | 成功、远端 typed error、本地 timeout/closed |
| 典型机器人数据 | Camera、LiDAR、IMU、Odometry | HealthQuery、RuntimeStatus、ConfigQuery |

## 为什么 Sensor Data 用 Pub/Sub

Sensor Data 通常具有高频、连续、可能多消费者和“新样本比旧样本更有价值”的特征。相机帧可以同时被定位、感知、记录器消费；传感器不应逐一等待所有消费者确认后再采下一帧。

Pub/Sub 允许每个 subscription 选择自己的 queue depth、overflow policy 与 QoS。慢消费者可以丢旧帧或新帧，而不会把传感器采集线程变成同步 RPC 链。consumer 临时不存在时，producer 仍能按传感器节拍工作。

把连续 Sensor Data 做成 RPC 会引入错误的耦合：采样周期被调用者延迟影响，fan-out 要由 caller 显式复制调用，任一 timeout 还会让“这次采样是否有效”变得含糊。

## 为什么 RuntimeStatus 可以用 RPC

`RuntimeStatus` 是低频、按需的一致快照。caller 在某个时刻提出一个具体问题，并需要：

- 一个与该 request 精确关联的 response；
- 明确的调用 deadline；
- 可区分的业务错误与 transport timeout；
- 返回后继续控制流的同步点。

这符合 Request/Response，而不是持续广播。若大量观察者需要连续状态流，仍应增加 status topic；RPC 不应承担高频 telemetry fan-out。

`HealthQuery` 与 `ConfigQuery` 同理：本阶段实现的是读取查询，不是持续传感器数据或配置变更事件。

## AutoRuntime 的 DDS RPC 路径

AutoRuntime 保留同一套 `Node::CreateService` / `Node::CreateClient` API。DDS adapter 在每个 service hash 对应的 request/response topic 上发送自定义 IDL：

```text
Client::Call
  -> RpcRequest(service, participant GUID, request_id, timeout budget, Message)
  -> DDS request topic
  -> Service callback group
  -> RpcResponse(correlation, StatusCode, detail, Message)
  -> DDS response topic
  -> pending request
```

topic hash 只生成合法且有界的 DDS topic 名；wire 中仍携带并校验完整 service name。client 只接受 participant GUID 与 `request_id` 同时匹配的响应。迟到响应找不到 pending entry，会被丢弃而不能满足后续调用。

这是 AutoRuntime 私有的 topic-based RPC v1，不是 OMG DDS-RPC，也不兼容 ROS 2 service wire protocol。

## 为什么 retry 可能造成 duplicate operation

timeout 只说明 caller 在 deadline 内没有收到 response。以下情况对 caller 看起来都一样：

1. request 根本没送达；
2. request 已送达但仍在 DDS 或 Executor queue；
3. handler 正在执行；
4. handler 已执行成功，但 response 丢失或迟到。

如果 caller 在第 3 或第 4 种情况下自动 retry，remote 可能再次执行同一操作。transport 的 `request_id` 只关联一次调用；新 retry 会得到新 request ID，因此不会自动去重。

本阶段 client 不自动 retry，也不宣称 exactly-once。

## Idempotency 在哪里需要

只读查询通常天然幂等：

- `HealthQuery` 重复读取不改变组件状态；
- `RuntimeStatus` 重复读取只产生新的快照；
- `ConfigQuery` 重复读取同一个 key 不产生副作用。

有副作用的操作必须在业务层设计幂等：

- `RestartNode` 可携带 operation id，并记录已完成 id；
- `SetConfig` 可使用 idempotency key 或 expected-version 的 compare-and-set；
- actuator command 应包含 command sequence，并由执行端拒绝重复或过期 sequence；
- 跨重启去重需要持久化记录，进程内 pending map 不够。

幂等边界属于业务状态机，因为只有业务知道“两次执行”是否等价。middleware 无法仅凭 payload 猜测。

## timeout 是否意味着 remote node 已死

不意味着。timeout 是一个本地观察：在 caller 的 monotonic deadline 前，没有得到可匹配的 response。它可能来自 discovery 尚未完成、网络丢包、DDS history 被占满、service queue 积压、handler 卡住、response 丢失，或 remote process 退出。

判断 peer death 必须组合独立证据，例如 DDS liveliness、AutoRuntime heartbeat、generation、process supervisor 与 no-progress 监控。即便这些信号也要区分“进程死”“进程活但 handler 无进展”和“网络分区”。

当前调用不存在 service 时也表现为 `Timeout`，不是 `NotFound`。远端 handler 主动返回的 `NotFound` 才是有来源的业务错误。

## QoS、队列与背压

RPC request/response endpoint 使用 Reliable、KeepLast、depth 64。它让短暂读写速度差有明确上界，但不等于无损或无限并发。超过 history/资源边界、writer blocking deadline 或 caller deadline，调用仍可能失败。

通过 `Node::Service` 时，业务 handler 还经过 callback group 的 64-entry bounded service queue。DDS history bound 与 Runtime queue bound 是两个不同层次，不能只观察其中一个。

## 当前限制

- 不自动 retry，不做 request dedup 或跨重启 exactly-once；
- missing service 没有 discovery-level negative acknowledgement；
- server 依据接收时的剩余 budget 建立本地 deadline；不同主机的 steady clock 不直接比较；
- 已验证同机两个真实 DDS participant，跨主机 RPC 证据仍是 **INCOMPLETE**；
- 不提供 authentication、authorization、encryption；
- Record/Replay 尚不记录 service/RPC；
- DDS receiver 与 vendor 内部线程还未纳入 AutoRuntime 的实时线程配置。
