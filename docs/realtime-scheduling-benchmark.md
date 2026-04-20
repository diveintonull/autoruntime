# 实时感知调度基准

## 1. 结论先行

实现提交 `0c0935c63b40` 已完成 Default、CPU affinity、CPU affinity + 请求 `SCHED_FIFO` 三种配置，并分别在 idle 与受控 CPU stress 下运行。

本机普通用户没有 `CAP_SYS_NICE`：两条请求 FIFO 的线程都返回 `EPERM(1)`，实际策略仍为 default。因此 Case A 与 Case B 有真实测量；Case C 的“权限不足仍可启动、fallback 可观测”已验证，但真实 FIFO 延迟对比标为 **INCOMPLETE**。

这次运行只有每场景一次、每次约 1 秒，并发生在 WSL2。它用于保存可复核证据和暴露方向，不是统计显著性研究，也不构成 hard real-time 证明。

## 2. 环境与方法

| 字段 | 值 |
| --- | --- |
| 日期 | 2026-08-21 |
| 源码 | `0c0935c63b40e8919bd10333ff0be4cace524d77` |
| CPU | Intel Core Ultra 9 275HX，WSL2 可见 24 CPU |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Compiler | GNU C++ 13.3.0 |
| Build | Release，Ninja |
| 周期 / deadline | 1000 µs / 1000 µs |
| 每场景时长 | 1000 ms |
| 时钟 | `std::chrono::steady_clock` |
| 分位数 | nearest-rank |
| stress | 每个允许 CPU 一条 pinned `SCHED_OTHER` busy 子进程，共 24 条 |

回调只做一次原子计数，主要观察 Runtime 释放与 dispatch 开销。stress 子进程在 Runtime 采样前 fork，CPU 用量不计入 `RUSAGE_SELF`；表中的 CPU、context switch 与 RSS 都只属于被测进程。

运行命令：

```bash
projects/autoruntime/build-verify-release/autoruntime_realtime_scheduling_benchmark \
  --duration-ms 1000 \
  --period-us 1000 \
  --output projects/autoruntime/docs/evidence/realtime-scheduling-2026-08-21-0c0935c63b40.json
```

## 3. Release jitter 结果

单位均为 µs；“FIFO 回退”行只是请求过 FIFO，实际仍为默认策略。

| 场景 | release | miss | P50 | P95 | P99 | P99.9 | MAX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Default / idle | 1000 | 0 | 113.302 | 186.227 | 296.614 | 461.711 | 476.993 |
| Affinity / idle | 1000 | 0 | 85.572 | 131.323 | 220.254 | 278.988 | 322.302 |
| Affinity + FIFO 请求 / idle（回退） | 1000 | 0 | 87.373 | 132.577 | 203.713 | 283.387 | 611.055 |
| Default / stress | 1003 | 22（2.2%） | 67.400 | 249.466 | 2958.678 | 4954.844 | 5941.337 |
| Affinity / stress | 1004 | 33（3.3%） | 66.091 | 500.497 | 3026.047 | 4953.387 | 5941.042 |
| Affinity + FIFO 请求 / stress（回退） | 1004 | 38（3.8%） | 66.403 | 552.932 | 4022.986 | 5957.137 | 6955.495 |

idle 的单次样本中，affinity 相比 Default 的 jitter P99 从 296.614 降到 220.254 µs（25.7%），P99.9 从 461.711 降到 278.988 µs（39.6%）。但 stress 下 affinity 的 miss 从 22 增至 33，P99 也略升；这说明绑核不是单调收益，目标 CPU 的竞争、迁移、IRQ 和虚拟化噪声都可能主导尾延迟。

FIFO 请求行不能与前两行当作 FIFO 效果比较，因为 JSON 明确记录 `fifo_applied=false`、`effective_policy=default`。

## 4. 资源指标

| 场景 | 进程 CPU | 自愿切换 | 非自愿切换 | MAX RSS |
| --- | ---: | ---: | ---: | ---: |
| Default / idle | 2.256% | 2002 | 0 | 3.75 MiB |
| Affinity / idle | 1.530% | 2003 | 0 | 3.75 MiB |
| FIFO 请求回退 / idle | 1.607% | 2003 | 0 | 3.75 MiB |
| Default / stress | 2.537% | 2166 | 382 | 3.75 MiB |
| Affinity / stress | 1.787% | 2903 | 1914 | 3.75 MiB |
| FIFO 请求回退 / stress | 1.682% | 2898 | 1917 | 3.94 MiB |

原始 JSON 还保存 execution time、response time、全部线程 requested/effective 状态、errno、queue overflow、skipped release 与 deadline event 数。当前六场景均无 queue overflow 或 skipped release。

## 5. 原始证据

- [原始 JSON](evidence/realtime-scheduling-2026-08-21-0c0935c63b40.json)
- SHA-256：`2925d5e5b6a5a862eeedb6ba01cbf61ed341a7c0b021aa8b892c04315517d7f5`
- 精确源码 revision：JSON 内嵌 `0c0935c63b40`

完整测试矩阵见 [testing.md](testing.md)，优先级继承实验见 [priority-inversion.md](priority-inversion.md)。

## 6. 仍需完成

- 在具有最小 `CAP_SYS_NICE` 权限的隔离 Linux 测试机上完成真实 Case C；
- 每场景进行多轮随机化顺序运行，报告中位数与 run-to-run 置信区间；
- 在目标车载/机器人硬件上控制 CPU governor、IRQ affinity、NUMA 与热状态；
- 加入符合实际 control workload 的 callback，而不只测空回调；
- 分离 bare-metal Linux 与 WSL2 虚拟化结果。

在这些工作完成前，项目定位保持为“实时感知与延迟确定性优化”，不写 hard real-time。
