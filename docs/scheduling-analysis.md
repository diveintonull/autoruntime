# 调度隔离分析

日期：2026-08-20

## 问题

300 ms 的 planning callback 是否会拖延 20 ms control loop？把两个 task 分配到独立 callback group 能否避免干扰？

## 实验

`autoruntime_scheduling_isolation_experiment` 用相同 workload 运行两次：

- `slow-planning` event callback sleep 300 ms；
- periodic `control-loop` 每 20 ms release，deadline 15 ms；
- shared scenario 中两个 task 共用一个 worker；
- isolated scenario 中 planning/control 各自使用 one-worker callback group；
- 在 300 ms 阻塞开始后 220 ms 采样 control progress；backlog drain 后再收集最终 timing sample。

复现：

```bash
cmake --build projects/autoruntime/build-dds-release \
  --target autoruntime_scheduling_isolation_experiment -j2
projects/autoruntime/build-dds-release/autoruntime_scheduling_isolation_experiment \
  --output projects/autoruntime/docs/evidence/scheduling-isolation-results.json
```

原始证据：[scheduling-isolation-results.json](evidence/scheduling-isolation-results.json)。

## 结果

| 指标 | Shared worker group | Isolated worker group |
| --- | ---: | ---: |
| 阻塞窗口内 control execution | 0 | 11 |
| Releases / finished | 21 / 21 | 21 / 21 |
| Deadline miss | 14 | 0 |
| Queue overflow | 0 | 0 |
| Response P50 | 80,162.030 us | 118.924 us |
| Response P95 | 260,159.457 us | 148.408 us |
| Response P99 | 280,159.195 us | 179.106 us |

Release run 使用 `e2e-latency-analysis.md` 中同一台未绑核 WSL2 host。它只有一次 run，microsecond 数值不是可移植性能保证。

## 为什么只提高 priority 没用

slow event 使用更高 queued priority，保证它先于第一次 periodic release 开始执行。callback 一旦运行，AutoRuntime 不会强制抢占。priority 只排序 callback-group queue 中等待的 job，无法中断任意 user code。

shared scenario 中，唯一 worker sleep 时 periodic release 继续产生。planning 返回后它们才执行，所以虽无 queue capacity loss，仍有 14 个 response 超过 15 ms。更深 queue 只保留旧 work，不保 timeliness。

isolated scenario 中，control worker 可独立运行，在 220 ms observation window 执行 11 次；考虑 startup phase 后符合预期 cadence，且没有 deadline miss。

## Runtime policy

实验支持以下部署规则：

1. latency-critical control/safety callback 放入专用 group；
2. blocking planning、logging、service handler 不得进入这些 group；
3. priority 只表示 queue ordering，不是 OS preemption；
4. 限制每个 task/subscription queue，并决定 stale work 是否仍有价值；
5. 监控 queue delay、response time、deadline miss、high watermark；
6. long callback 应与 `std::stop_token` 合作，但不能假设 cancellation 能终止不合作代码；
7. 只有在 target Linux kernel/hardware 上测量后，才使用 OS affinity 与 real-time scheduling。

## 局限

测试用 `sleep_for` 制造可重复阻塞，不是 CPU contention。它没有测 cache interference、external lock priority inversion、page fault、IRQ load、CPU frequency change 或 RT throttling。这些需要绑核 worker、target hardware 与 kernel scheduler evidence。
