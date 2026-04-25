# rclcpp 对比基线

这个目录是可选的 ROS 2 package，不进入 AutoRuntime 默认 CMake graph。它只用于在同一硬件、相同两进程 request/echo 拓扑、相同 payload、频率、持续时间和 QoS 下生成 `rclcpp + DDS` 基线。

消息使用 `std_msgs/msg/ByteMultiArray`，QoS 固定为 Reliable、KeepLast(64)。每条 payload 都完整写入、在 responder 完整校验、复制到 response，并在 initiator 再次完整校验；不是只传 offset 的伪大消息测试。
每个 case 的 responder 都在 `fork` 后立即 `exec /proc/self/exe`，再在全新地址空间初始化 ROS context。不能在父进程跑过 DDS 线程后，让下一次 fork 的 child 直接继续调用 RMW；那会继承 middleware 的进程全局状态与 mutex，连续 case 会在 setup 阶段失败。


## 构建

在已 source ROS 2 环境后：

```bash
mkdir -p /tmp/autoruntime-rclcpp-ws/src
ln -s \
  /workspace/projects/autoruntime/benchmarks/rclcpp_baseline \
  /tmp/autoruntime-rclcpp-ws/src/autoruntime_rclcpp_baseline
cd /tmp/autoruntime-rclcpp-ws
colcon build \
  --packages-select autoruntime_rclcpp_baseline \
  --cmake-args -DCMAKE_BUILD_TYPE=Release
source install/setup.bash
```

连续 case 回归测试：

```bash
colcon test --packages-select autoruntime_rclcpp_baseline
colcon test-result --verbose
```

## 运行

```bash
ros2 run autoruntime_rclcpp_baseline \
  rclcpp_comparative_benchmark \
  --frequency 100 \
  --duration-ms 3000 \
  --trials 3 \
  --payload 64 \
  --payload 1024 \
  --payload 65536 \
  --payload 1048576 \
  --output /workspace/rclcpp-results.jsonl
```

使用 Cyclone DDS 时显式设置：

```bash
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
```

环境记录同时包含实际加载的 `rmw_get_implementation_identifier()` 与请求的 `RMW_IMPLEMENTATION`，不能只凭环境变量推断运行时实现。构建时可通过 `-DRCLCPP_BASELINE_SOURCE_REVISION=<commit>` 注入被测 AutoRuntime 提交号。若没有安装并验证 `rclcpp`、`std_msgs`、RMW implementation 或 ROS 2，本 baseline 必须标为 **INCOMPLETE**，不能用 AutoRuntime DDS 数字代替。

跨机器运行不复用这个单机 fork runner；feature 9 会使用独立 initiator/responder role。当前 runner 的 DDS 范围是同机两个进程。
