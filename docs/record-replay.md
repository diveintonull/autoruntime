# Record / Replay 设计

## 1. 目标与边界

Record/Replay 用于把偶发输入和 transport publish attempt 保存为可校验 trace，再在新的 Runtime 实例中按原始节奏或受控节奏重新注入。它解决“相同输入能否重新执行”的证据链，不承诺任意多线程、浮点、GPU 或外部 I/O 应用天然确定。

本阶段记录 pub/sub publish attempt；service/RPC request/response 尚未纳入 trace。Recorder 在 transport 调用前入队，因此 strict 模式可以在无法记录时阻止 publish。若底层 transport 随后失败，trace 仍会保留这次 attempt；它不是 confirmed-delivery 日志。

## 2. 公共模块

`record_replay/include/autoruntime/record_replay.hpp` 提供：

- `Recorder`：异步有界队列与单 writer thread；
- `RecordingTransport`：包裹任意 `Transport`，记录 publish attempt，其余 API 透明转发；
- `Replayer`：校验并顺序读取 trace；
- `ReplayTiming::Original/Accelerated/AsFastAsPossible`；
- `Next()`：逐条 step mode。

Recorder queue 固定容量。Block 模式有明确 enqueue timeout；DropNewest 模式返回 `Dropped`。统计公开 accepted、written、dropped、timeout、I/O error、queue high watermark 与 bytes written。

## 3. 文件格式

文件和 record 都使用显式 little-endian 编码，不把 C++ struct padding 直接落盘。

文件头包含：

```text
magic
format_version
header_size
wall_clock_start_ns
monotonic_start_ns
reserved
header_crc32
```

每条 record 包含：

```text
record_magic / record_version / total_size
record_sequence
monotonic_offset_ns
MessageEnvelope 全字段
channel_length / message_type_length / payload_length
channel
message_type
payload
record_crc32
```

`wall_clock_start_ns` 只用于把会话关联到人类时间；重放顺序和节奏只使用 `steady_clock` 对应的 monotonic offset。reader 校验 magic、version、长度上限、整数溢出、CRC、record sequence 单调递增和 timestamp 不回退。未知版本、截断或损坏会返回 typed error，不做 silent best effort。

## 4. Recorder 线程与所有权

调用线程深拷贝 channel、message type、envelope 和 payload 到 bounded queue；writer thread 是文件句柄的唯一 owner。这样磁盘 I/O 不在实时 callback 上执行，但 payload copy 与入队锁仍是可测干扰，不能称为零开销。

`Stop(deadline)` 先拒绝新记录，再要求 writer drain、flush 并结束。deadline 超时不会 detach writer；调用者可以稍后继续等待，析构函数最终执行无限等待，避免后台线程访问已销毁状态。

## 5. RecordingTransport 失败语义

`RequireRecord`：

1. 先把 publish attempt 入 Recorder；
2. 入队失败则不调用底层 Transport；
3. 入队成功后再调用底层 Transport。

`ContinueWithoutRecord`：记录失败仍继续 publish，并累计 `record_failures`。这种模式优先可用性，但 trace 可能有洞，不能用于“完整输入重放”的证明。

## 6. Replay 语义

- Original：第一条 record 立即交付，后续保持相对 monotonic 间隔；
- Accelerated：将相对间隔除以大于 1 的 speed；
- AsFastAsPossible：不等待，主要用于 correctness 与吞吐测试；
- Step：调用 `Next()` 一次取一条。

Replay callback 返回非 OK 时立即停止；stop token 在等待期间也会被轮询。统计包含 records read/delivered、payload bytes、checksum/sequence/truncation/format error，以及 timing drift observation。

## 7. 验证

- format round-trip：完整 envelope、channel、type、payload、Reset 与 EOF；
- corruption/truncation：CRC 与短读必须拒绝；
- transport failure：strict 不得在 trace failure 后继续 publish，continue 模式要显式计数；
- timing modes：原速间隔、加速模式和 step mode；
- 5000 输入集成：Record → 销毁 Runtime → 新建 Runtime → Replay；
- trace integrity：15000 条 Sensor/Planning/Control record 逐字段、逐 payload 比较；
- application determinism：相同 Sensor trace 经过 deterministic mock pipeline 后，5000 个 Control payload 与 digest 相同；
- 轻依赖默认构建 35/35；DDS Debug、Release、ASan、UBSan 各 37/37；
- TSan 下 Record/Replay 专项 5/5；
- **INCOMPLETE：** 完整 DDS TSan 第二次重跑为 36/37，失败发生在外部 Cyclone DDS 11.0.1 的 participant-loss 用例，报告 `libddsc.so.11` 内部 SPDP buffer 的非确定性竞态；未过滤或伪装为通过。
