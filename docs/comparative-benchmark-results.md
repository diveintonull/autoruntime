# 同机统一对比基准结果

日期：2026-08-21
被测实现：`d25967384beb0c01bb47bbdb689220b9a0cee25e`

## 结论先行

本轮在同一台 WSL2 host 上完成了 AutoRuntime FastIPC copy、FastIPC loan、AutoRuntime Cyclone DDS 和 `rclcpp + Cyclone DDS` 的双进程 request/echo 对比。所有路径都完整写、读并校验 payload，不是只传共享内存 offset。

- 默认 64 B、1 KiB、64 KiB、1 MiB，100 Hz：FastIPC copy/loan 共 24/24 个 trial 完整成功；rclcpp DDS 12/12 完整成功；AutoRuntime DDS 11/12 完整成功，1 MiB 的第三次 trial 完成 294/300，丢失 6 个响应。
- 4 MiB、25 Hz 稳态：四条路径共 12/12 个 trial 均完成 300/300，零 payload mismatch、零 publish failure、零 unexpected response。
- 4 MiB、100 Hz 压力：FastIPC copy/loan 共 6/6 个 trial 均完成 300/300；AutoRuntime DDS 三次完成 198、210、199/300，合计丢失 293；rclcpp DDS 三次完成 216、209、188/300，合计丢失 287。
- 4 MiB、25 Hz 的三 trial 中位数：FastIPC loan 的 P50/P99 为 7.050/7.955 ms，FastIPC copy 为 13.935/15.317 ms；这是当前机器、当前实现和当前 full-touch workload 的观测值，不外推为通用 middleware 加速比。
- 100 Hz DDS 失败组的延迟只来自已返回响应，不能解释成成功负载下的稳态 latency；它们是容量/积压证据。

实际 DRAM memory bandwidth 没有硬件计数器证据，当前只有 request + response 的 `logical_payload_mib_per_second`，因此“真实内存带宽”仍为 **INCOMPLETE**。跨主机 DDS 对比也仍为 **INCOMPLETE**。

## 方法与环境

| 字段 | 值 |
| --- | --- |
| Host CPU | Intel Core Ultra 9 275HX，24 logical CPUs |
| Kernel | WSL2 `6.6.87.2-microsoft-standard-WSL2` |
| Compiler / build | GNU 13.3.0 / Release |
| 拓扑 | 同机两个真实进程，request/echo |
| QoS | Reliable、KeepLast、depth 64 |
| 时钟 | initiator `steady_clock` RTT |
| 发布节拍 | open-loop absolute release |
| payload 验证 | producer 完整写；responder 完整校验并 echo；initiator 完整校验 |
| 重复 | 每个 mode/payload 三个 trial；表格取三次中位数，原始三次均保留 |
| AutoRuntime DDS | Cyclone DDS 11.0.1 |
| rclcpp DDS | ROS 2 Jazzy；Cyclone DDS 0.10.5；`rmw_cyclonedds_cpp` 2.2.3 |
| rclcpp image | 本地镜像 `sha256:b7c0ce7d65a38844190e76a7c0880c31e08946f566fccea932f5734583fa81e0` |

rclcpp 路径运行在固定 Docker 用户态，AutoRuntime 路径运行在 WSL2 用户态；两者共享硬件和 kernel，但 DDS 版本、ROS executor、消息表示、allocator 与容器用户态不同。因此这里只比较完整路径，不能把 DDS 路径之间的差值归因于某一个 framework。

## 默认负载：100 Hz

下表延迟单位为 ms。`完成` 按 trial 顺序列出；P50/P95/P99/P99.9/MAX、CPU 与资源数据均为三个 trial 的中位数。

| 路径 | Payload | 成功 trial | 完成 | 丢失合计 | P50 | P95 | P99 | P99.9 | MAX |
| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| FastIPC copy | 64 B | 3/3 | 300/300/300 | 0 | 0.170 | 0.268 | 0.333 | 0.486 | 0.486 |
| FastIPC loan | 64 B | 3/3 | 300/300/300 | 0 | 0.166 | 0.274 | 0.380 | 0.557 | 0.557 |
| AutoRuntime DDS | 64 B | 3/3 | 300/300/300 | 0 | 1.520 | 2.170 | 2.306 | 2.363 | 2.363 |
| rclcpp DDS | 64 B | 3/3 | 300/300/300 | 0 | 0.282 | 0.445 | 0.605 | 0.685 | 0.685 |
| FastIPC copy | 1 KiB | 3/3 | 300/300/300 | 0 | 0.171 | 0.302 | 0.384 | 0.559 | 0.559 |
| FastIPC loan | 1 KiB | 3/3 | 300/300/300 | 0 | 0.157 | 0.263 | 0.423 | 0.813 | 0.813 |
| AutoRuntime DDS | 1 KiB | 3/3 | 300/300/300 | 0 | 1.465 | 2.185 | 2.307 | 2.321 | 2.321 |
| rclcpp DDS | 1 KiB | 3/3 | 300/300/300 | 0 | 0.288 | 0.416 | 0.508 | 0.690 | 0.690 |
| FastIPC copy | 64 KiB | 3/3 | 300/300/300 | 0 | 0.307 | 0.462 | 0.532 | 0.755 | 0.755 |
| FastIPC loan | 64 KiB | 3/3 | 300/300/300 | 0 | 0.275 | 0.423 | 0.531 | 0.741 | 0.741 |
| AutoRuntime DDS | 64 KiB | 3/3 | 300/300/300 | 0 | 1.760 | 2.363 | 2.540 | 2.739 | 2.739 |
| rclcpp DDS | 64 KiB | 3/3 | 300/300/300 | 0 | 0.537 | 0.739 | 0.870 | 0.997 | 0.997 |
| FastIPC copy | 1 MiB | 3/3 | 300/300/300 | 0 | 3.359 | 3.676 | 3.885 | 4.491 | 4.491 |
| FastIPC loan | 1 MiB | 3/3 | 300/300/300 | 0 | 1.861 | 2.060 | 2.227 | 3.215 | 3.215 |
| AutoRuntime DDS | 1 MiB | 2/3 | 300/300/294 | 6 | 45.580 | 278.627 | 306.561 | 324.785 | 324.785 |
| rclcpp DDS | 1 MiB | 3/3 | 300/300/300 | 0 | 6.023 | 67.453 | 110.807 | 128.349 | 128.349 |

资源表中的 CPU 是两个进程 CPU time / 测量与 drain wall time；可以超过 100%。context switch 为 initiator + responder 的增量。RSS 单位为 KiB，格式为 initiator/responder。

| 路径 | Payload | CPU % | Context switch V/I | Peak RSS KiB I/R |
| --- | ---: | ---: | ---: | ---: |
| FastIPC copy | 64 B | 1.409 | 1550/0 | 4608/2716 |
| FastIPC loan | 64 B | 2.247 | 1549/0 | 143044/5092 |
| AutoRuntime DDS | 64 B | 3.669 | 7230/0 | 143044/6700 |
| rclcpp DDS | 64 B | 2.800 | 1571/0 | 15552/15360 |
| FastIPC copy | 1 KiB | 1.450 | 1550/0 | 4608/2740 |
| FastIPC loan | 1 KiB | 2.257 | 1550/0 | 143044/5092 |
| AutoRuntime DDS | 1 KiB | 3.616 | 7093/0 | 143044/7000 |
| rclcpp DDS | 1 KiB | 2.967 | 1572/0 | 15552/15552 |
| FastIPC copy | 64 KiB | 2.979 | 1550/0 | 12480/11052 |
| FastIPC loan | 64 KiB | 4.108 | 1549/0 | 143044/13164 |
| AutoRuntime DDS | 64 KiB | 6.324 | 7184/0 | 143044/13040 |
| rclcpp DDS | 64 KiB | 5.580 | 1574/0 | 21516/21708 |
| FastIPC copy | 1 MiB | 33.303 | 1550/0 | 141168/137844 |
| FastIPC loan | 1 MiB | 31.312 | 1550/2 | 143044/136036 |
| AutoRuntime DDS | 1 MiB | 55.384 | 16392/5 | 143044/54908 |
| rclcpp DDS | 1 MiB | 50.094 | 12749/6 | 40580/36256 |

`initiator_peak_rss_kib` 来自长生命周期 initiator 的 `ru_maxrss`，因此会保留同一次 runner 中更早 case 的高水位并受 case 顺序影响；例如 loan 的小 payload 行不能解释成它为 64 B 分配了 143044 KiB。responder 每 case 都是新进程，口径更接近单 case peak。RSS 已记录，但当前 initiator peak 不适合做跨行 footprint 结论。

## 4 MiB 稳态：25 Hz

每次 trial 仍发送 300 条消息，因此窗口改为 12000 ms；不是用更少样本换取“通过”。

| 路径 | 成功 trial | 完成 | 丢失 | P50 ms | P95 ms | P99 ms | P99.9/MAX ms | CPU % | Context V/I | Peak RSS KiB I/R |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| FastIPC copy | 3/3 | 300/300/300 | 0 | 13.935 | 14.740 | 15.317 | 16.020 | 35.282 | 1693/0 | 553344/551196 |
| FastIPC loan | 3/3 | 300/300/300 | 0 | 7.050 | 7.519 | 7.955 | 8.514 | 30.365 | 1693/1 | 561360/535404 |
| AutoRuntime DDS | 3/3 | 300/300/300 | 0 | 16.553 | 120.514 | 225.389 | 276.558 | 46.943 | 39032/37 | 561360/117980 |
| rclcpp DDS | 3/3 | 300/300/300 | 0 | 15.216 | 59.317 | 118.516 | 191.153 | 45.091 | 18977/27 | 91992/78680 |

在这个未丢响应的 operating point，FastIPC loan 相比 FastIPC copy 的中位 P50、P99 和 CPU 都更低；同时 small payload 结果表明 loan 并非在所有维度都自动占优。结论限定为这套实现与 full-touch workload。

## 4 MiB 压力：100 Hz

| 路径 | 成功 trial | 完成 | 丢失合计 | P50 ms | P95 ms | P99 ms | P99.9/MAX ms | CPU % | Context V/I | Peak RSS KiB I/R |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| FastIPC copy | 3/3 | 300/300/300 | 0 | 14.315 | 15.029 | 15.331 | 16.097 | 143.504 | 1550/2 | 553228/551196 |
| FastIPC loan | 3/3 | 300/300/300 | 0 | 7.182 | 7.956 | 8.960 | 10.788 | 123.168 | 1551/26 | 553228/535344 |
| AutoRuntime DDS | 0/3 | 198/210/199 | 293 | 1847.304 | 3402.900 | 4091.429 | 4092.841 | 89.223 | 110915/169 | 553228/453736 |
| rclcpp DDS | 0/3 | 216/209/188 | 287 | 1809.773 | 3304.327 | 3678.881 | 3684.020 | 90.625 | 154854/170 | 379024/459076 |

DDS 两组的 CPU 分母包含额外 5 秒 drain，不能与 3 秒内完成的 FastIPC CPU 百分比直接解释为“更省 CPU”。压力组证明在这组 QoS、频率和 full-touch 工作量下发生积压与未完成响应；它不证明 DDS 的最大吞吐，也不代表其他 DDS 配置或实现。

## 可复核原始证据

| 文件 | 结果分布 | SHA-256 |
| --- | --- | --- |
| [AutoRuntime 默认矩阵](evidence/comparative-2026-08-21-d259673-autoruntime.jsonl) | 35 ok / 1 failed；lost 6 | `b38b1458c650aeb18981e3b7e0c3fa2158a168128e76e3bc56a79bd98c97fa5a` |
| [AutoRuntime 4 MiB 25 Hz](evidence/comparative-2026-08-21-d259673-autoruntime-4m-25hz.jsonl) | 9 ok；lost 0 | `edebb4e7265619d203adad00010faef92563327106e5c1e4f86526391759b1ed` |
| [AutoRuntime 4 MiB 100 Hz](evidence/comparative-2026-08-21-d259673-autoruntime-4m-100hz.jsonl) | 6 ok / 3 failed；lost 293 | `7dcff015fae40cbf294109aa9b9c6a187420f7546cc6bd5208c3a861d6cda4d8` |
| [rclcpp 默认矩阵](evidence/comparative-2026-08-21-d259673-rclcpp.jsonl) | 12 ok；lost 0 | `be868cefd6c0095285a078c8b8f4946930ea0603fbef437a3584f6d67e7fe33d` |
| [rclcpp 4 MiB 25 Hz](evidence/comparative-2026-08-21-d259673-rclcpp-4m-25hz.jsonl) | 3 ok；lost 0 | `1a9e8f0a8c681e33af30abeb32b61adf2b22da9f4c289f897fe0e60735e58e23` |
| [rclcpp 4 MiB 100 Hz](evidence/comparative-2026-08-21-d259673-rclcpp-4m-100hz.jsonl) | 3 failed；lost 287 | `9b86f685f1ce8d29da615f68506586ec5704531931d4a4b2ed5a6c5ad6f57c77` |

机器审计逐行解析了全部 JSON，确认 `schema_version=1`、Release build、`source_revision=d25967384beb`、统一 `comparison_group=same_host_two_process_request_echo`，且 72 个 result 中 payload mismatch、publish failure 和 unexpected response 均为 0。

## 解释边界与后续工作

- 原始文件保存每个 trial；表格的三次中位数只是摘要，不能代替原始尾延迟分布。
- 单 trial 只有 300 个样本，P99.9 基本等于 MAX，不是稳定 SLO 估计。
- 默认 rclcpp 1 MiB 三个 trial 的 P99 波动很大，不能只引用中位数掩盖第三次的长尾；原始文件保留全部数据。
- 真实 DRAM bandwidth、cache miss、NUMA、CPU pinning 与 idle/stress 分层尚未测量。
- AutoRuntime DDS 与 rclcpp DDS 使用不同 Cyclone DDS 版本；本报告不宣称 framework 本身带来某个百分比提升。
- 当前只有 same-host 证据。network delay、disconnect/reconnect、peer disappear、RPC timeout 和 discovery recovery 留给第 9 项 Cross-machine Stability，状态为 **INCOMPLETE**。
