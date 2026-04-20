# 优先级反转与优先级继承实验

## 1. 问题

经典优先级反转包含三条固定在同一 CPU 的线程：

1. 低优先级线程先持有共享 mutex；
2. 高优先级线程随后请求同一 mutex 并阻塞；
3. 中优先级线程执行长时间 CPU 工作。

没有优先级继承时，中优先级线程可以抢占低优先级线程，使高优先级线程间接等待中优先级工作。启用 `PTHREAD_PRIO_INHERIT` 后，高优先级 waiter 会临时提升 mutex owner，目标是让低优先级线程尽快退出临界区。

## 2. 实现

公共 `RealtimeMutex` 满足 BasicLockable 接口。Linux 构造时：

- 普通模式使用默认 pthread mutex；
- PI 模式设置 `pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)`；
- 初始化结果通过 `priority_inheritance_requested/active/error` 查询；
- Executor 的 topology、task 与 callback-group mutex 使用同一包装。

源码：

- `scheduler/include/autoruntime/realtime_mutex.hpp`
- `scheduler/src/realtime_mutex.cpp`
- `scheduler/src/executor.cpp`

## 3. 实验设计

`benchmarks/priority_inversion_experiment.cpp` 依次运行 PI OFF 与 PI ON：

- low：`SCHED_FIFO/10`，持锁 busy 20 ms；
- medium：`SCHED_FIFO/20`，busy 100 ms；
- high：`SCHED_FIFO/30`，测量请求 mutex 到获得 mutex 的阻塞时间；
- 三条线程固定到进程 cpuset 中同一个 CPU。

实验保存每条线程的 requested/effective policy、priority、affinity、errno 与 detail。只有以下条件全部成立，`comparison_valid` 才为 true：

- OFF/ON 两轮三条线程 affinity 都真实生效；
- OFF/ON 两轮三条线程 `SCHED_FIFO` 都真实生效；
- PI ON 轮的 mutex 确认 `PTHREAD_PRIO_INHERIT` 已启用。

## 4. 为什么权限回退不能算结果

普通用户通常缺少 `CAP_SYS_NICE`，`pthread_setschedparam` 会返回 `EPERM`。此时三条线程都处于 `SCHED_OTHER`，Linux CFS 的运行顺序不再对应 low/medium/high 的实时优先级关系。

因此，即使两轮都能得到一个 `high_blocking_us` 数字，也不能据此声称 PI 改善或恶化。程序会输出：

```text
comparison_valid = false
invalid_reason = SCHED_FIFO and affinity must apply ...
```

本环境的经典 PI 对比状态标为 **INCOMPLETE**，直到在具备受控实时调度权限的机器上重跑并得到 `comparison_valid=true`。这不影响 mutex 属性初始化、能力查询和普通并发正确性的单元测试通过。

## 5. 运行方法

```bash
cmake -S projects/autoruntime \
  -B projects/autoruntime/build-verify-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_BUILD_BENCHMARKS=ON
cmake --build projects/autoruntime/build-verify-release \
  --target autoruntime_priority_inversion_experiment
projects/autoruntime/build-verify-release/autoruntime_priority_inversion_experiment
```

不要直接给整个 shell 提权。若要完成有效对比，应在隔离测试机上授予最小的 `CAP_SYS_NICE`，同时确认 cpuset、CPU governor、IRQ 与系统负载，并保留原始 JSON。

## 6. 解释边界

- PI 只处理“高优先级线程等待低优先级 mutex owner”这一类阻塞。
- PI 不能缩短临界区，也不能解决 I/O、page fault、allocator、IRQ 或回调自身超时。
- PI 会增加 mutex 内核 bookkeeping；短临界区是否值得启用需要实测。
- 本项目提供 real-time-aware 优化，不提供 WCET、准入控制或 hard real-time 保证。
