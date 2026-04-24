# AutoRuntime 架构

## 设计目标

AutoRuntime 把应用语义、调度、transport、恢复、可观测性和小型 distributed control plane 分开。目标是通过窄而清晰的 public seam，让 ownership 与 failure behavior 可检查，而不是藏在 monolithic middleware singleton 后面。

## 模块地图

| 模块 | Public seam | 主要实现 | 职责 |
| --- | --- | --- | --- |
| Runtime | `runtime/include/autoruntime/node.hpp` | `runtime/src/node.cpp` | Node-scoped endpoint、subscription/service queue、timer、envelope |
| Scheduler | `scheduler/include/autoruntime/executor.hpp` | `scheduler/src/executor.cpp` | task release、priority dispatch、callback group、cancellation、sample |
| Transport | `transport/include/autoruntime/transport.hpp` | `transport/src/*` | protocol-neutral pub/sub 与 request/reply contract |
| Health | `health/include/autoruntime/health_monitor.hpp` | `health/src/health_monitor.cpp` | state evaluation、generation、restart budget、hook |
| Observability | `observability/include/autoruntime/observability.hpp` | `observability/src/observability.cpp` | metric、JSON log、span、process usage |
| Distributed | `distributed/include/autoruntime/distributed.hpp` | `distributed/src/*` | bounded discovery 与 framed RPC |

## 数据路径

1. `Publisher` 构造 versioned `MessageEnvelope`，带 trace/span id、sequence、monotonic timestamp、source generation 和 priority。
2. 选定的 `Transport` 发布消息，并负责 wire/shared-memory encoding、具体 QoS mapping 与 transport counter。
3. transport subscription callback 调用 `Subscriber::Impl::Accept`。
4. subscriber 应用自己的 bounded queue 与显式 DropNewest/DropOldest policy。
5. 每个 subscription 最多安排一个 Event task。`ProcessOne` 取出一条消息、执行 application callback；仍有 backlog 时再次安排自己。
6. Executor 独立于 transport metric，采集 release、queue、execution、response 和 deadline timing。

这种 transport/subscription 两阶段边界让 slow application callback 不会阻塞 transport receive thread。

## Transport 能力矩阵

| 能力 | In-memory | FastIPC | Cyclone DDS |
| --- | --- | --- | --- |
| Pub/sub | 支持 | 支持，configured SPSC endpoint | 支持 |
| Service/client | 支持 | 不支持 | 支持，自定义 topic-based RPC v1 |
| Cross-process | 否 | 同一 Linux host | DDS domain；RPC 当前仅同机验证 |
| Queue/QoS mapping | runtime + local bus | reliable -> bounded timeout；best effort -> drop | reliability/history/deadline/liveliness |
| 具体 lifetime | shared bus state | receiver `jthread` + FastIPC channel | participant + pub/sub/RPC endpoint + receiver `jthread` |

公共接口暴露 service method；当前 FastIPC 明确返回 typed `Unsupported`，而不是假装所有 adapter 能力相同。

## DDS Request/Response deep module

`dds_rpc_engine` 隐藏 IDL、topic 命名、participant GUID、request ID、pending map、endpoint QoS 与 receiver thread。`DdsTransport` 只委托 `AdvertiseService`、`RemoveService`、`Request` 和统计 hook，因此 pub/sub 路径不需要理解 RPC correlation。

每个 service hash 对应独立 request/response topic，但 wire 仍携带并校验完整 service name。client 在 publish 前插入 pending entry；response 只有在 participant GUID、service 与 request ID 全部匹配时才能完成 waiter。timeout 会移除 pending，迟到 response 只能计入 drop。

client absolute monotonic deadline 覆盖 write 与 wait。wire 发送剩余 budget，server 在本机重建 handler deadline，避免跨主机比较 `steady_clock` time point。关闭时先阻止新 write、把 pending 完成为 `Closed`、停止并 join receiver，再删除 endpoint，最后才删除 participant。

协议是 AutoRuntime topic-based RPC v1，不是 OMG DDS-RPC 或 ROS 2 service wire protocol。它不自动 retry、去重或承诺 exactly-once。

## Scheduler 模型

scheduler thread 释放 periodic job；Event job 由 `Notify` 释放；Async job 在加入已经运行的 executor 时释放一次。每个 callback group 拥有 bounded priority queue 和配置好的 worker set。排序依次按 priority 降序、release time、insertion sequence。

task capacity 与 group capacity 分别检查。overflow 返回 `QueueFull` 并增加 task counter。cancellation 设置 stop source、阻止未来 dispatch，并给正在运行的 cooperative callback 提供 `stop_token`。每个 task 最多保留最近 4096 个完成 sample。

## 所有权与生命周期

- caller 拥有共享 `Executor` 与 concrete `Transport`。
- `Node` 对二者保持 shared ownership；endpoint handle 持有 shared PImpl。
- subscription/service transport callback 只捕获 weak endpoint state，避免 callback deregistration 形成 reference cycle。
- endpoint destructor 调用幂等 `Close`/`Cancel`。
- 文档要求的 callback group 与 task 必须在 `Start` 前配置；executor 停止后不能再次启动。
- concrete transport 先标记 closed、唤醒 pending、停止并 join receiver thread、关闭/删除 substrate，再进入析构。

## 健康与恢复

`HealthMonitor` 的 component state 与 execution/transport object 分离。它评估 process liveness、heartbeat freshness、progress、backlog 和 deadline miss。每次 update 携带 generation；stale generation 不能复活 replacement process。恢复会让失败 component 进入 `Recovering`，在 monitor lock 外执行 application-owned cleanup/start/reconnect hook，推进 generation，再进入 `Starting`，直至新的 heartbeat/progress 证明 `Running`。

已验证的 SIGKILL 流程见 [recovery.md](recovery.md)。

## 可观测性

scheduler sample、subscription stat、transport stat、health transition、metric、structured log、trace span 和 process resource usage 是彼此独立的数据产品。benchmark 为每个 trace 组合三个 span 来推导 sensor-to-control latency，而不是从 wall-clock log timestamp 猜测延迟。

## Distributed control plane

discovery 向显式 bounded peer list 发送 versioned UDP announcement。record 按 node id 索引，并由 generation + heartbeat sequence fencing；lease 会移除沉默成员。RPC 使用 size-bounded versioned TCP frame、nonblocking I/O、monotonic deadline、cancellation polling 与 typed response status。

该 distributed slice 有意不做通用 cluster manager。

## Shutdown 顺序

安全的应用 shutdown：

1. 停止新的 publication 与 timer；
2. 关闭 subscription/service，使 transport callback 注销；
3. 请求 executor stop，让 cooperative callback 返回；
4. 关闭 concrete transport 并 join receiver thread；
5. 停止 discovery/RPC，并按需持久化最终 metric。

`Executor::Stop` 目前 join 时不会执行传入 deadline，因此应用必须保证 callback 合作退出。

## 不变量

- Node generation 非零，并随每个 published envelope 传递。
- subscription queue 与 callback-group queue 都有上界。
- transport callback 不得强持有 application endpoint lifetime。
- stale health generation 不得修改 current state。
- discovery membership 与 RPC frame 具有显式 size bound。
- DDS response 不得完成 GUID/request ID 不匹配或已经 timeout 的 pending call。
- request ID 只负责 correlation，不得被描述成业务 operation idempotency key。
- 导入的 `tcp_pubsub` 不进入默认 AutoRuntime target。

## 非目标

本实现不提供 hard real-time scheduling、zero-copy DDS loan、OMG DDS-RPC/ROS 2 service wire compatibility、exactly-once RPC、process-manager daemon、secure discovery、distributed consensus 或当前 envelope 之外的 schema evolution。
