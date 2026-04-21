# 确定性边界

## 1. 两个不同问题

### Transport Replay Integrity

trace 保存 channel、message type、record sequence、monotonic offset、完整 `MessageEnvelope`、payload size、payload 与 CRC32。读取后逐字段和逐字节比较，可以回答：

> 写入文件的 transport message 是否被无损读回？

这个结论不依赖应用算法是否确定。

### Application Determinism

对当前 deterministic mock pipeline，测试固定整数变换、单 worker 顺序和无随机源：

```text
同一 Sensor 输入 trace
→ 同一 Planning payload
→ 同一 Control payload digest
```

测试比较 5000 个输出 payload 与 digest。trace/span id 属于运行时观测 metadata；新一轮执行可能分配新的 span id，因此应用 digest只覆盖业务 payload，不把新生成的 tracing metadata 伪装成算法输出。

## 2. 为什么 Replay 不自动等于 Deterministic

真实算法仍可能受以下因素影响：

- 浮点归约顺序、FMA 与编译器选项；
- callback 调度和锁竞争；
- 未固定 seed 的随机数；
- wall clock、环境变量、文件和网络输入；
- GPU kernel、驱动与异步 completion 顺序；
- 未记录的配置、模型版本与外部状态。

因此项目只对测试覆盖的 mock pipeline 声明 bit-exact output payload，不声称未来所有应用天然 deterministic。

## 3. 时钟

文件头同时保存 wall clock 与 monotonic clock 起点：

- wall clock 用于定位录制会话；
- monotonic offset 用于排序和 replay timing；
- wall clock 跳变不会改变 record 顺序或相对间隔。

Original timing 以第一条 record 为零点，保持 record 间相对间隔，不重放录制启动到第一条消息之间的空闲。

## 4. 证据要求

最终报告必须同时给出：

- records written/read/delivered；
- payload mismatch；
- sequence mismatch；
- output digest mismatch；
- checksum/truncation/format error；
- replay timing drift；
- exact source revision 与原始证据文件。

若任一完整性检查失败，结果标记失败；若应用不满足确定性前提，只报告 transport integrity，不外推 application determinism。
