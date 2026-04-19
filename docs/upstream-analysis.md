# AutoRuntime 上游分析

## 范围与证据

AutoRuntime 以 eclipse-ecal/tcp_pubsub 的提交 `1540876ee8aad623a9b089baaf3f948579b466d9` 为起点。它是基于 standalone Asio 的紧凑 C++ TCP binary-blob pub/sub library。准确历史、许可证与 tree hash 见工作区 [UPSTREAMS.md](../../../UPSTREAMS.md)。

在固定 submodule 下，干净的 Release/Ninja baseline 通过全部五个上游 CTest。这只证明选定 transport baseline 可构建，不证明任何新 scheduler、health、observability 或 DDS requirement。

## 上游是否定义 Node？

没有。public object 是 Executor、Publisher、Subscriber、SubscriberSession。publisher 监听 TCP endpoint，subscriber 连接 endpoint list。它没有 node identity、lifecycle、topic registry、component graph、service/client abstraction、timer 或 runtime coordinator。

## 通信如何工作？

publisher 接受 TCP session，把每个 binary payload 序列化为 packed little-endian `TcpHeader`，再和复制的 caller byte 一起放入 pooled vector。header 只有 header size、content type、reserved byte 与 payload size。一字节 handshake 当前只协商 protocol version zero。

每个 publisher session 使用 Asio strand，并只允许一个 write in flight。后续 send 会覆盖一个 `next_buffer_to_send_` slot。这是隐式 latest-value/drop-intermediate policy，不是可配置 backpressure。

subscriber 解析有序 publisher list、连接、handshake，读取 variable header 与 payload，并在 strand 上投递。失败后关闭 socket，每秒重试一次，直至耗尽 budget。它能在已配置 endpoint 间 failover，但没有 discovery 或 membership protocol。

## Executor 如何运行？

`Executor_Impl` 持有一个共享 `asio::io_context`、work guard，以及 N 个执行 `io_context::run` 的 thread。它只是 I/O completion pool：没有 task model、priority queue、periodic release、deadline、admission、affinity、cancellation token 或 runtime statistic。

`Stop` 重置 work guard 并停止 context。implementation destructor 会 detach worker，因为每个 worker 捕获 implementation 的 shared pointer。这样避免 self-join，却没有向 caller 提供明确的 quiesce、drain、join contract。

## Callback 在哪个线程运行？

- synchronous mode 在 session 的 Asio strand 上直接执行 user callback，即使用 executor I/O worker。下一次 read 随后才安排。slow callback 会占用 I/O pool，拖延无关 networking。
- asynchronous mode 创建一条 subscriber callback thread。callback 执行期间，新消息会覆盖单个 `last_callback_data_` slot。它隔离 I/O，却会静默折叠 burst，且没有 queue depth、deadline、priority 或 drop metric。

替换或取消 callback 会 join 线程；如果调用发生在该线程自身，则上游会 detach。

## 如何选择 Transport？

无法选择：TCP 被编译进 publisher、subscriber、protocol 与 endpoint API。没有 Transport seam，也没有 shared-memory、UDS、DDS 或 mock backend。eCAL sample 是外部 adapter，不是 runtime abstraction。

## Shutdown 如何工作？

publisher cancellation 会关闭/cancel acceptor、标记 stopped、在 mutex 下复制 session list，再 cancel session。subscriber cancellation 会 cancel session 并停止 callback thread。session 会 cancel socket、retry timer 与 resolver。共享 executor 停止 I/O context。

它没有 ordered runtime shutdown、drain deadline、callback cancellation propagation、generation retirement、health transition，也不能保证 public object 返回时 detached worker 已退出。

## 如何处理故障？

socket/protocol error 会记录日志。subscriber session 关闭后重试；publisher session 在 I/O error 后移除。protocol version 大于零会拒绝。这能处理 connection-level recovery，但无法区分 process crash、stale instance、no progress、backlog 或 deadline overrun。没有 generation、heartbeat、lease、restart coordinator 或结构化的 application-visible failure reason。

receive path 会按远端声明的 payload length reserve/resize buffer。重设计必须在分配前执行 configured frame bound。

## QoS、Metric 与调度

- QoS：没有 reliability/history/depth/deadline/liveliness model。TCP 提供 ordered reliable byte，但 latest-slot overwrite 仍然隐式。
- Metric：只有 subscriber count；没有 message rate、depth、drop、callback latency、task timing、CPU 或 E2E latency。
- Scheduling：没有 application scheduler。strand 只串行化 session，不实现 priority、period、deadline 或 isolation group。
- Tracing：有 logger callback；没有 trace identity、sequence、timestamp 或 structured event schema。

## 修改边界

### 保留

- MIT license、Continental copyright、`portable_endian.h` notice 与历史。
- 已知可工作的 CMake/CTest scaffold，以及抽取期间选定的 TCP primitive。
- endpoint failover behavior 和 basic、large-message、multi-peer、failover regression test。
- buffer reuse 与 Asio ownership 思路，但只有新 failure/lifetime test 证明正确的部分才可继续采用。

### 重写

- public API：改为 Node、Publisher、Subscriber、Service、Client、Timer、Executor、Transport、HealthMonitor。
- Executor：改为带 priority、period、deadline、bounded queue、cancellation、callback group、timing metric 的 periodic/event/async task。
- latest-value slot：改为显式 per-subscription queue 与 backpressure，记录 drop reason、watermark，并隔离 slow callback。
- TCP framing：改为 bounded versioned envelope，携带 sequence、monotonic timestamp、trace identity、scheduling metadata、request correlation 与 error status。
- Recovery：改为 generation-aware state machine，区分 disconnect、peer replacement、stale generation 和 no progress。
- Shutdown：改为 stop-accepting、cancel、drain、close、joined-worker 阶段。

### 移除

- runtime product surface 中的 eCAL tunnel sample。
- 把隐式 one-slot overwrite 当作唯一 queue policy。
- application-facing component API 中的 TCP endpoint type。
- 把 detached worker/callback lifetime 当作正常 shutdown。
- 不服务于聚焦 Linux runtime 的 compatibility/build surface。

移除不会抹去归属；导入 commit 与 notice 仍保留在历史中。

### 新增

- 围绕统一 API 的 runtime coordinator 与 component lifecycle。
- 同机高速通信使用的 project-local FastIPC adapter。
- 跨 host 与 ROS integration 使用成熟 ROS2/DDS client-library adapter；AutoRuntime 不重写 DDS。
- reliability/best-effort、history/depth、deadline、liveliness policy mapping，以及可复现 QoS experiment。
- heartbeat、generation、state、progress、backlog、deadline overrun、crash detection 与 reconnect/restart coordination。
- structured log、metric、trace/sequence/timestamp，以及 Sensor-to-Control P50/P95/P99 measurement。
- 有限 node discovery、heartbeat membership 与 RPC，不假装实现 consensus system。
- 自动化 scheduler、malformed-message、disconnect、restart、slow-callback、shutdown-under-load、DDS-peer-loss fault 与 sanitizer build。
