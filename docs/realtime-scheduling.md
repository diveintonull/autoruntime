# AutoRuntime 实时感知调度设计

## 1. 定位与非目标

AutoRuntime 提供的是 real-time-aware runtime：通过线程放置、可选 Linux 实时策略、绝对周期释放、deadline action 和分位数证据减少延迟抖动。它不提供 WCET 证明、准入控制、锁页、IRQ 隔离、NUMA 控制或经过认证的内核，因此不得称为 hard real-time system。

本阶段只配置 Executor 创建的 scheduler thread 与 callback-group worker。FastIPC/DDS receiver、HealthMonitor、Discovery 和 RPC 自有线程尚未接入同一配置面；文档和 benchmark 不把局部线程配置外推为“整个进程实时化”。

## 2. 配置边界

`ExecutorConfig` 配置 scheduler thread，`CallbackGroupConfig::thread_scheduling` 配置该组所有 worker。应用可以表达：

- planning group 固定到一组 CPU；
- control group 固定到另一组 CPU，并请求 `SCHED_FIFO`；
- monitor group 使用默认 Linux 调度；
- scheduler thread 独立配置 CPU 与策略。

`ThreadSchedulingConfig` 包含 CPU ID 集合、`Default`/`Fifo` 策略和 FIFO priority。配置来自创建 Executor/CallbackGroup 的 runtime setup，不在实现中硬编码。

CPU ID 在创建或启动前校验。Linux worker 启动后调用 `pthread_setaffinity_np`、`pthread_setschedparam`，随后用 get API 读取实际值。每条线程状态同时保存 requested、effective、errno 与 fallback 标志。

## 3. 权限不足与降级

普通用户通常没有 `CAP_SYS_NICE`，请求 `SCHED_FIFO` 时 `pthread_setschedparam` 可能返回 `EPERM`。该错误不阻止 Runtime 启动：

1. worker 保持系统实际调度策略；
2. startup snapshot 标记 realtime fallback；
3. benchmark 把该场景记录为 fallback，不能写成 FIFO 成功；
4. 无效 priority 或非法 CPU ID 属于配置错误，不作为权限降级吞掉。

affinity syscall 的环境错误同样保留在状态中。明确的“已请求但未生效”比静默继续更可诊断。

## 4. 周期释放与漂移

周期任务的基准时刻在 `Start` 时建立：

```text
next_release = start + period
release(next_release)
next_release += period
```

等待使用 steady clock 的 absolute time point。回调执行时间和一次调度延迟不会被加到下一周期，因此不会产生 `sleep_for(period)` 式累计漂移。落后过多时仍有有界 catch-up；达到上限后记录 skipped release，并从第一个严格晚于当前时刻的原始周期网格继续，不改成 `now + period`。

## 5. 调度样本与统计

每个已观察 job 记录：

- `scheduled_release_time`；
- `actual_start_time`；
- `finish_time`；
- release jitter = actual start - scheduled release；
- execution time = finish - actual start；
- response time = finish - scheduled release；
- queue delay = actual start - enqueue time；
- deadline miss 与采取的 action。

每个 task 对 retained sample window 汇总 P50/P95/P99/P99.9/MAX；同时保留累计 release、miss、drop、overflow 与 callback failure counter。miss rate 的分母是已经完成或在 dispatch 前丢弃的 deadline observation，不把尚未执行的队列项伪装成成功。

## 6. Deadline policy

### Warn

回调照常执行。miss 增加 warning counter，并向可选 `DeadlineEventHandler` 发事件。handler 可接 StructuredLogger，但异常会被隔离并计数，不能杀死 worker。

### Degrade

回调照常执行。miss 除通知外还把 `RuntimeStats::degraded` 置为 true；应用确认处置后可显式 acknowledge。HealthMonitor 的集成通过事件 handler 调用 `ReportDeadlineMisses`，Executor 不直接拥有 deployment health state。

### Drop

worker 在执行前检查 `now > scheduled_release + deadline`。如果 stale work 尚未开始，则不调用 callback，记录零 execution time、deadline miss 和 drop。已经开始后才超时的回调不能被安全强杀，只记录 warning；该策略不声称抢占任意 C++ 用户代码。

## 7. Priority inheritance

Executor 的 scheduler、task 与 group 元数据会被不同优先级线程短暂共享。Linux 上使用带 `PTHREAD_PRIO_INHERIT` 的 BasicLockable 包装；不支持的平台退回普通 mutex 并暴露能力状态。用户在多个 callback group 之间共享锁时也可以显式使用该包装。

独立 priority-inversion 实验只在 affinity 与三条 `SCHED_FIFO` 线程都真实生效时比较 PI OFF/ON。权限不足时输出 `INCOMPLETE`，不使用 `SCHED_OTHER` 数字冒充经典优先级反转证据。

## 8. 验证计划

- 单元测试：配置校验、CPU affinity、FIFO fallback、P99.9/MAX、absolute release grid；
- failure test：dispatch 前 stale job 的 Drop、Degrade 状态、deadline handler exception isolation；
- integration test：planning/control 分组与 Node timer 保持兼容；
- benchmark：Default、Affinity、Affinity + FIFO，分别在 idle 与受控 CPU stress 下运行；
- sanitizer：Debug、Release、ASan、UBSan、TSan 全矩阵；
- 文档：保存原始 JSON、命令、环境、权限状态和限制。

## 9. 代码与测试锚点

- 配置、状态与 deadline 类型：`scheduler/include/autoruntime/executor.hpp`
- Linux affinity/FIFO、absolute release、deadline action：`scheduler/src/executor.cpp`
- PI mutex：`scheduler/include/autoruntime/realtime_mutex.hpp`、`scheduler/src/realtime_mutex.cpp`
- 单元/集成/failure 测试：`tests/executor_test.cpp`
- 六场景基准：`benchmarks/realtime_scheduling_benchmark.cpp`
- 经典优先级反转实验：`benchmarks/priority_inversion_experiment.cpp`

CTest 另外注册四个可独立定位的用例：`thread_scheduling`、`deadline_policy`、`absolute_release`、`priority_inheritance`。最终的 exact-revision 原始数据、环境与权限状态写入独立基准文档；只有输出中的 `effective_policy=fifo` 才能算作 Case C。
