# DDS Request / Response 设计

## 1. 目标与边界

本阶段让现有 `Node::CreateService`、`Node::CreateClient` 能通过真实 Cyclone DDS 11.0.1 adapter 工作，不新增第二套 Runtime API。至少以 `HealthQuery`、`RuntimeStatus`、`ConfigQuery` 三个控制面服务完成端到端验证。

本实现使用 DDS topic 组合 Request/Response，不宣称实现 OMG DDS-RPC 标准，也不声称兼容 ROS 2 service wire protocol。它只保证 AutoRuntime participant 之间的版本化协议。

## 2. 协议与 topic

IDL 新增 `RpcRequest` 与 `RpcResponse`，和普通 pub/sub `Message` 使用独立 type descriptor 与 topic：

```text
autoruntime_rpc_request_v1_<service hash>
autoruntime_rpc_response_v1_<service hash>
```

request 至少包含：

- protocol version；
- 128 B 上限的 service name；
- 128-bit DDS participant GUID；
- 非零单调 `request_id`；
- client 剩余 timeout budget；
- 原始 Message envelope 与 payload。

response 回显 service、client GUID、`request_id`，并携带 `StatusCode`、有界 error detail 和成功 Message。topic hash 只用于得到合法、有限长度的 DDS 名称；接收端仍比较完整 service name，不能把 hash 当身份或授权。

## 3. Client correlation

每个 transport 的 participant GUID 是 client instance identity，`request_id` 是该 instance 内从 1 开始的原子序列。client 必须先把 pending entry 插入 map，再 publish request，避免极快 response 先于 waiter 注册。

response receiver 只完成三元组完全匹配的 pending call：

```text
(client_guid_high, client_guid_low, request_id)
```

timeout 会原子移除 pending entry；迟到 response 只能被计为 dropped，不能完成后续 request。该机制解决 correlation，不提供 operation deduplication。

## 4. Deadline 与 error

client 以调用者的 monotonic absolute `Deadline` 控制整个 write + wait。wire 只发送 publish 时的剩余 budget，不发送跨机器不可比较的 `steady_clock::time_point`。server 收到后在自己的 monotonic clock 上建立 local deadline；因此 client deadline 是最终 E2E 边界，而 server budget 主要限制排队/handler 等待。

`Deadline::Infinite` 在 wire 中用 `UINT64_MAX` 表示。transport `Close` 必须把所有 pending call 完成为 `Closed` 并唤醒无限等待者。

handler error 通过 `StatusCode + detail` 返回。接收端必须校验 status code 范围、协议版本、service、GUID、request ID、envelope version、payload/detail 长度；非法样本不得完成 pending call。

## 5. Service execution

每个 advertised service 有独立 request reader、response writer 与接收线程。线程只负责 DDS take、协议校验、调用已有 `TransportServiceCallback`、写 response。通过 `Node::Service` 时，真正业务 handler 仍进入配置的 Executor callback group 与 64-entry bounded queue；DDS adapter 不绕过 scheduler。

`RemoveService` 先从公开 map 移除，再请求线程停止、等待退出并删除 DDS endpoints。重复 service name 返回 `AlreadyExists`，重复 remove 返回 `NotFound`。

## 6. 并发与关闭

同一 client channel 支持多个并发 request，pending map 由 mutex 保护，response thread 逐条完成 waiter。DDS write 使用独立 mutex 与 closed check；Close 在删除 participant 前阻止新 write、停止并 join 所有 RPC receiver，并唤醒 pending。

普通 pub/sub 与 RPC 使用不同 endpoint/type，现有 pub/sub hot path 和 QoS contract 不改变。

## 7. 失败语义

- 没有匹配 service：client 到 deadline 后返回 `Timeout`；这不证明 remote node 已死。
- response error：client 返回远端 `StatusCode/detail`，不伪装成 transport error。
- client timeout 后 response 到达：作为迟到样本丢弃，不能误配下一请求。
- participant `Close`：未完成 request 返回 `Closed`。
- handler throw：server 转成 `Internal`。
- DDS write/take error：映射到 typed transport status，并增加失败/丢弃计数。

## 8. Retry 与 idempotency

client timeout 时不知道 request 是未送达、排队中、执行中，还是 response 丢失。自动 retry 可能执行同一业务两次；本阶段不自动 retry。

`HealthQuery`、`RuntimeStatus`、`ConfigQuery` 是只读、天然适合 retry 的查询。写配置、触发动作、资金或状态转换必须由业务层使用 idempotency key、compare-and-set 或去重表；transport 的 `request_id` 只做单次调用关联，不能替代业务幂等。

## 9. 非目标

- OMG DDS-RPC / ROS 2 service wire compatibility；
- exactly-once operation execution；
- 跨重启 request deduplication；
- 身份认证、授权与加密；
- 由 timeout 推断 peer death；
- 把 DDS vendor 内部线程声明为 AutoRuntime real-time worker。
