# AutoRuntime 上游与衍生实现边界

## 目的

本文件区分保留的 `tcp_pubsub` 历史、AutoRuntime 原创工作和重写工作。它是作者归属与维护地图，不是科研新颖性声明。

## 来源与法律边界

- Primary upstream：[eclipse-ecal/tcp_pubsub](https://github.com/eclipse-ecal/tcp_pubsub)
- 固定上游提交：`1540876ee8aad623a9b089baaf3f948579b466d9`
- 本地 subtree 导入提交：`f00857d`
- 许可证：MIT；上游版权保留于 `LICENSE`
- 额外保留声明：`tcp_pubsub/src/portable_endian.h`

导入保留上游源码、sample、测试、submodule metadata 和历史。没有 vendor 两个 secondary reference 的代码。

## 上游提供了什么

固定基线提供基于 Asio 的 binary TCP publisher/subscriber、connection handshake/framing、executor thread pool、endpoint failover、single next-message slot、CMake/package scaffold、example，以及 basic、large-message、multi-publisher/subscriber 和 failover 测试。

它不提供 AutoRuntime Node API、task model、callback group、显式 per-subscription queue、service/client/timer API、transport-neutral contract、FastIPC/DDS adapter、health/recovery state machine、runtime metric、structured tracing、bounded membership、distributed RPC、fault matrix 或三组实验。

## 当前构建边界

默认 `autoruntime` target 只编译新 `runtime`、`scheduler`、`health`、`observability`、`distributed`、`transport` module。启用时链接相邻 FastIPC target；Cyclone DDS 可选且默认关闭。

导入的 `tcp_pubsub`、sample 与 test 仍保留，用于 provenance 和 baseline comparison。只有在 `AUTORUNTIME_BUILD_TCP_BASELINE=ON` 且固定 submodule 可用时，它们才进入 target graph。因此默认 runtime 不是对导入 library 的薄包装。

## Keep / Rewrite / Add

### 保留并注明来源

| 项目 | 处理方式 |
| --- | --- |
| MIT license、Continental copyright、portable_endian notice | 原文保留 |
| 完整上游 source/history/test/sample | 作为 baseline subtree 保留 |
| TCP handshake/framing 与 failover test | 仅用于 baseline/reference |
| CMake 仓库结构与 platform workflow artifact | 保留；根 CI 已覆盖其职责 |

### 重写或替换

| 层面 | 基线 | AutoRuntime |
| --- | --- | --- |
| Public API | TCP publisher/subscriber class | 统一 Node/Pub/Sub/Service/Client/Timer/Executor/Transport/HealthMonitor |
| Execution | 通用共享 Asio thread pool | Periodic/Event/Async task、callback group、priority、bound、cancellation、sample |
| Queue semantics | 隐式 single next-message slot | per-subscription depth、DropNewest/DropOldest、high watermark、drop |
| Callback behavior | socket delivery callback | transport callback -> bounded subscription queue -> executor callback |
| Transport coupling | public surface 暴露 TCP type | protocol-neutral interface 与 typed unsupported capability |
| Lifecycle | connection-level ownership | endpoint RAII、cooperative executor stop、transport close/join、generation fencing |
| Build | baseline library 与 example | 默认构建新 library；导入 baseline 需显式启用 |

### 衍生项目新增

| 能力 | 证据 |
| --- | --- |
| In-memory runtime service 与 pub/sub | `runtime/`、`transport/src/in_memory_transport.cpp`、runtime test |
| FastIPC cross-process adapter 与真实 restart flow | `transport/src/fastipc_transport.cpp`、`tests/fastipc_transport_test.cpp` |
| Cyclone DDS 11.0.1 pub/sub adapter 与 QoS mapping | `transport/src/dds_transport.cpp`、DDS test/experiment |
| 原创 DDS topic-based Request/Response engine | `transport/src/dds_rpc_engine.cpp`、RPC IDL、correlation/failure test、独立 JSONL benchmark |
| Health evaluation 与 recovery hook | `health/`、`docs/recovery.md` |
| metric、JSON log、span、E2E latency、CPU metric | `observability/`、benchmark evidence |
| Bounded UDP discovery 与 framed TCP RPC | `distributed/`、distributed test |
| 20 个命名 runtime fault case | `tests/fault_injection_test.cpp`、`docs/fault-injection.md` |
| 五种 profile 的 sanitizer/build matrix 与根 CI | script、workflow、`docs/testing.md` |

## 定量审阅辅助

在 implementation checkpoint `fc8150d`：

- 新 production module 的 `.cpp`、`.hpp`、`.idl` 共 5,758 行物理源码；
- 衍生测试、benchmark 和可运行 example 共 3,975 行；
- 统一口径统计的保留上游 C/C++ baseline 共 4,334 行；
- `git diff --shortstat f00857d:projects/autoruntime fc8150d:projects/autoruntime` 报告 56 个文件变化、11,002 行新增、74 行删除。

这些数字只辅助理解范围，不是作者比例。diff 包含文档与生成的测量记录；diff 之外保留的上游文件仍属于完整构建历史。

## 增量提交证据

| Commit | 引入的边界 |
| --- | --- |
| `f6a7571` | 上游架构审计与修改计划 |
| `781a3cf` | 统一 runtime API 与 executor |
| `d410c9e` | cross-process FastIPC adapter |
| `9fd7aed` | 真实 Cyclone DDS adapter |
| `e0c6648` | DDS QoS 测量实验 |
| `b5551ed` | health monitor 与 recovery policy |
| `c7554d0` | metric、structured log、trace、CPU data |
| `43179db` | sensor-planning-control E2E benchmark |
| `88121eb` | bounded discovery 与 distributed RPC |
| `7b7dbc5`、`cd9a571` | 显式 runtime 与 delivery fault matrix |
| `cd976bb` | callback-group isolation experiment |
| `ef1a334` | 根 CI 与五 profile verification |
| `8372865` | 可运行 pipeline example |
| `31b2107` | close/unmap lifetime fix 与真实 post-restart message proof |
| `fc8150d` | 轻依赖默认 build；DDS 仍为已验证的 opt-in |

## 次要参考

- [gazebosim/gz-transport](https://github.com/gazebosim/gz-transport)：用于比较 Node/pub-sub/service seam、discovery behavior 与 test taxonomy。
- [shawnfeng0/uorb](https://github.com/shawnfeng0/uorb)：用于比较 queue depth、callback/poll semantics 与 sanitizer organization。

这里没有编译或复制两个仓库的源码。

## 明确不作的声明

AutoRuntime 不声称 ROS 2 API/service wire compatibility、OMG DDS-RPC、DDS implementation ownership、exactly-once RPC、hard real-time scheduling、process-daemon supervision、secure cluster membership、zero-copy message、production certification，也不声称与 ROS 2、Apollo Cyber RT、iceoryx、eCAL 或完整 DDS stack feature parity。记录的 WSL2 实验只代表本实现的比较证据。
