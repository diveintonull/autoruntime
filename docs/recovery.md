# 健康管理与恢复设计

## 状态模型

```text
STARTING --heartbeat/progress--> RUNNING
RUNNING  --backlog/deadline----> DEGRADED
DEGRADED --signals clear------> RUNNING
*        --exit/lease/stall----> FAILED
FAILED   --Recover()----------> RECOVERING
RECOVERING --hooks succeed----> STARTING (generation + 1)
RECOVERING --hook/timeout-----> FAILED
```

`HealthState` 与 `HealthReason` 分离。因此 component 可以表达 `FAILED/ProcessExited`、`FAILED/HeartbeatLost` 或 `DEGRADED/BacklogExceeded`，无需把 reason 塞进自由文本。

## 检测

`Evaluate` 先在 monitor mutex 下 snapshot probe request，释放 mutex 后执行可能阻塞的 process probe，最后仅在 PID 与 generation 仍匹配时应用结果。评估顺序：

1. process exit；
2. recovery timeout；
3. heartbeat timeout；
4. no-progress timeout；
5. backlog bound；
6. deadline-miss bound；
7. degraded-to-running recovery。

heartbeat 与 progress sequence 必须递增。每次 update 都携带 registered generation；旧 generation 或未注册的 future generation 返回 `StaleGeneration`。

## Recovery transaction

`Recover` 只接受仍有 restart budget 的 `Failed` component。它在 monitor lock 下预留 next generation，并把状态改为 `Recovering`，然后在 lock 外执行三个 application-owned hook：

1. `cleanup(name, old_generation)`
2. 等待 configured restart backoff
3. `start(name, new_generation) -> pid`
4. `reconnect(name, new_generation)`

异常会被捕获并转换为 typed status。reconnect 失败会对 replacement generation 尽力 cleanup。只有 component 仍持有 old generation 且仍为 `Recovering` 时才能 commit success，否则返回 `StaleGeneration`。component 随后以清空的 heartbeat/progress/backlog/deadline counter 进入 `Starting`。

monitor 不自行 fork process。hook ownership 把 deployment policy 留在可复用 state machine 之外。

## 真实 SIGKILL 与数据流测试

`tests/fastipc_transport_test.cpp` 执行完整同机恢复：

1. parent 创建 camera generation 9 的 FastIPC publisher；
2. exec 出来的 child 创建 planning generation 7 subscriber；
3. `before-crash` 通过 shared memory，且 envelope 得到校验；
4. parent 发送 `SIGKILL` 并 reap child；
5. `HealthMonitor::Evaluate` 观察到 `FAILED/ProcessExited`；
6. recovery hook 启动 generation 8 的新 exec child；
7. reconnect 发布 `after-restart`；
8. replacement 证明 receiver generation 8、source generation 9、sequence 2、payload 与非零 trace id；
9. generation 8 的 heartbeat 与 progress 让 health 回到 `RUNNING`；
10. replacement 与 publisher 干净关闭。

Release test 连续运行十次，结果见 [evidence/recovery-repeat-10.log](evidence/recovery-repeat-10.log)。

## 测试发现的真实缺陷

更强的第一版测试恢复了消息流，但 replacement 偶尔在 shutdown 时崩溃。ASan 报告 `munmap` 后读取：FastIPC receiver thread 仍在从 blocked `Receive` 展开调用栈，另一线程的 `Close` 已释放 mapping。

commit `31b2107` 用 in-process operation lease 修复 substrate：

1. closure 拒绝新 lease；
2. 两个 futex epoch 都递增并 wake；
3. active operation 观察 `Closed`，随后释放 lease；
4. `Close` 等待 active count 归零；
5. 最后才释放 role metadata、mapping 与 fd。

FastIPC 现有直接 regression `LocalCloseWaitsForBlockedOperationBeforeUnmapping`；AutoRuntime crash/restart test 覆盖完整 adapter lifecycle。修复后五种 sanitizer/build profile 全部通过。

## 所有权与并发

- `HealthMonitor` 只拥有 state、policy、transition history 和 probe callable。
- application/deployment code 拥有 process handle 与 hook side effect。
- monitor mutex 持有期间绝不运行 hook。
- generation 是 logical incarnation fence；PID 只是一项 liveness observation。
- FastIPC 还独立使用 PID、process start tick、channel generation、role token 对 substrate role fencing。
- 本切片的 recovery 同步执行；生产 supervisor 应放在专用 control-plane executor。

## 局限

- 没有 process daemon、cgroup/systemd integration、exponential backoff、rolling restart、dependency graph 或持久化 restart budget。
- hook 可能阻塞超过 caller deadline，因为 C++ 不能抢占任意 synchronous function；deadline 只在 hook stage 之间检查。
- 默认 process probe 使用 `kill(pid, 0)`，本身不能防 PID reuse；验证路径依靠 generation 和 FastIPC start-tick check。
- recovery 只建立 `Starting`，不代表立即健康；必须收到 fresh heartbeat 与 progress。
- 没有 multi-node leader election 或 exactly-once restart semantics。
