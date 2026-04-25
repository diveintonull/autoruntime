# AutoRuntime 与 FastIPC 零复制集成

## 状态与范围

AutoRuntime 的同机 FastIPC 路径已经提供端到端的 `Loan -> Publish -> Take -> Release`。应用直接写共享 chunk 的 payload 区，订阅 callback 在共享 chunk 仍被占用时读取该区域；runtime 不再创建中间 `std::vector<std::byte>`。

这里的“零复制”只描述 AutoRuntime 与 FastIPC 之间的 payload ownership 路径，不代表：

- 应用算法不读写内存；
- DDS、in-memory transport 或 Record/Replay 已支持 loan；
- 操作系统、网卡或序列化栈完全没有复制；
- callback 可以在返回后继续持有裸 `span`。

DDS 与 in-memory transport 对 loan API 返回 typed `Unsupported`。DDS loan、跨 transport fallback 和 Recorder 直接消费 loan 都仍是 **INCOMPLETE**。

## 为什么需要独立能力接口

普通发布接口接收 `span<const std::byte>`，transport 必须把调用方内存复制到自己的存储。若只在 FastIPC adapter 内部换成 loan，runtime 仍会先构造 `Message::payload`，大消息路径依然发生一次完整复制。

因此 public seam 增加两组 move-only handle：

- `PublisherLoan` / `TransportPublisherLoan`：独占尚未发布的 producer reservation；
- `LoanedMessage` / `TransportSubscriberSample`：独占尚未 release 的 consumer sample。

concrete transport 通过私有 `Backend` 实现 ownership。Node API 不暴露 FastIPC 类型，也不把 loan 支持伪装成所有 transport 的公共最小能力。

## 发布路径

```cpp
auto result = publisher.Loan(frame_bytes);
if (!result) {
  return result.status();
}

auto loan = std::move(result).take_value();
FillCameraFrame(loan.Data());
return loan.Publish();
```

状态与所有权：

```text
FastIPC FREE
  -> RESERVED（FastIPC producer loan 独占）
  -> 应用写入 payload，consumer 不可见
  -> Publish 写入 AutoRuntime envelope header
  -> FastIPC PUBLISHED，producer handle 失效
```

`Publisher::Loan` 在 runtime 层分配 sequence、trace/span 与 source generation；`Publish` 在真正提交前更新 publish monotonic timestamp，并把 80-byte wire header 直接写入共享 chunk。payload span 从 header 后开始，因此没有 staging vector。

`Publish`、`Abandon` 和析构三者最多消费 reservation 一次。move assignment 会先归还目标对象原有 reservation。未显式发布的 handle 在析构时自动 `Abandon`。

## 订阅路径

```cpp
auto subscriber = node.CreateLoanedSubscriber(
    "camera/frame", options,
    [](const autoruntime::LoanedMessage& frame) {
      ProcessFrame(frame.Data());
    });
```

状态与所有权：

```text
FastIPC PUBLISHED
  -> Take 得到 SubscriberSample
  -> adapter 只解码并复制固定 envelope header
  -> sample 进入 subscription bounded queue
  -> application callback 读取共享 payload
  -> callback 返回，LoanedMessage RAII release
  -> FastIPC slot 可回收
```

`LoanedMessage::Data()` 的有效期只到 callback 返回或所属 sample 被提前销毁。callback 参数是 const reference，不能复制 handle。若应用把裸指针或 `span` 保存到 callback 外，属于 use-after-release；接口不承诺其有效性。

subscription queue 可以暂存 move-only sample，因此 slow callback 会真实占用 chunk，而不是偷偷复制后立即释放。这使背压、pool exhaustion 与 RSS/延迟测量反映实际 ownership。

## 队列与背压

FastIPC endpoint 与 runtime subscription queue 是两个不同上界：

1. FastIPC chunk pool 限制跨进程在途、已发布和被 consumer 持有的 sample；
2. subscription queue 限制 transport callback 已接收、尚未执行的 application callback。

Reliable QoS 映射为 FastIPC bounded timeout；BestEffort 映射为 immediate drop。默认可靠发送 deadline 在 QoS 未给出时使用 adapter 的有限默认值。

必须区分两类失败：

| 场景 | FastIPC 状态 | AutoRuntime 状态 | 含义 |
| --- | --- | --- | --- |
| 同一 publisher 已有未完成 reservation | `WouldBlock` | `QueueFull` | 单 producer loan 规则被占用，不等待自身释放 |
| 所有 chunk 被发布/consumer 持有 | `Timeout` | `Timeout` | 在 QoS deadline 内没有可回收 chunk |
| BestEffort 无可用 chunk | `Dropped` | `Dropped` | 明确允许丢弃 |
| payload 超过 endpoint 上限 | 参数错误 | `InvalidArgument` | 不截断，不隐式分配 |
| transport 已关闭 | `Closed` | `Closed` | handle 不可再使用 |

adapter 的 `send_mutex` 只保护 `channel->Loan()` 调用，不覆盖应用持有 loan 的整个时间。否则第二个线程会在普通 mutex 上无期限等待，绕过 QoS deadline。

## Slow callback 与释放顺序

`LoanedSubscriber::Impl::Accept` 在 transport receiver thread 中只完成：

1. 检查 closed；
2. 应用 DropNewest/DropOldest；
3. move sample 入有界队列；
4. 最多通知一个 executor Event task。

应用 callback 在 executor worker 中执行，不阻塞 receiver thread。但它会占用 sample 对应的共享 slot；这是预期背压语义。DropNewest 会立即析构新 sample，DropOldest 会析构最旧 sample，两者都会通过 RAII 释放 chunk。

关闭顺序为：标记 subscription closed、清空队列并释放 sample、unsubscribe 并 join receiver、cancel executor task。transport callback 只捕获 weak state，避免 callback registration 与 endpoint 形成强引用环。

## Generation 与崩溃恢复边界

AutoRuntime handle 不重新实现 FastIPC 的 generation/liveness 协议。共享 chunk 的 producer/consumer owner PID、owner generation、channel generation、stale-handle fencing 和 peer-death reclaim 由 FastIPC substrate 承担。AutoRuntime envelope 另外携带 Node source generation，用于应用消息语义与恢复观测，不能替代 chunk generation。

正确分层是：

- FastIPC generation 决定旧 handle 能否发布或释放共享 slot；
- AutoRuntime source generation 决定上层消息是否来自当前 Node incarnation；
- adapter 只把 FastIPC typed failure 映射为 runtime status，不通过“重启清空全部共享内存”规避问题。

FastIPC 项目的 crash-after-loan、consumer-death 与 stale-generation 测试是 substrate 证据；AutoRuntime 的 loaned integration test 证明 adapter 没有破坏该 ownership。

## Transport 能力矩阵

| 能力 | In-memory | FastIPC | Cyclone DDS |
| --- | --- | --- | --- |
| copy publish/subscribe | 支持 | 支持 | 支持 |
| producer loan | `Unsupported` | 支持 | `Unsupported` |
| callback-scoped loaned sample | `Unsupported` | 支持 | `Unsupported` |
| slow callback 占用 substrate storage | 否 | 是 | 未实现 |
| payload 上限 | 由内存决定 | endpoint 配置 | config 与 IDL bound |

## 测试

`tests/loaned_fastipc_transport_test.cpp` 注册以下独立 case：

- `unsupported`：in-memory 的 producer/subscriber loan 均返回 `Unsupported`；
- `end_to_end`：1 MiB payload 完整写入、完整校验，检查 sequence、generation 与统计；
- `backpressure`：两 slot pool、slow callback、pool timeout、held reservation `QueueFull`、`Abandon` 后 reclaim。

`autoruntime_comparative_benchmark` 另外以两个真实进程测量 FastIPC copy 与 loan，并要求 producer、responder、initiator 三处 full-touch。性能数字只在固定 Release 提交和原始 JSONL 都存在时写入分析文档。

## 已知限制

- 当前 FastIPC AutoRuntime adapter 仍是每 endpoint 单 publisher/单 subscriber；MPMC 是 FastIPC 的独立 channel 类型，尚未暴露为 runtime adapter。
- subscription callback 不能异步延长 sample 生命周期；如需跨 callback ownership，应设计显式 retain budget，而不是保存裸 span。
- Record/Replay 包装 transport 会复制 payload，尚未支持 move-only sample。
- 4 MiB DDS bound 已为联合实验放宽，但 DDS 仍走序列化/copy 路径。
- 该实现优化同机大消息数据路径，不构成 hard real-time 或 lock-free runtime 声明。
