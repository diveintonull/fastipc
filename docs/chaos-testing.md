# FastIPC Seeded Chaos / Soak

## 目标与非目标

`fastipc_chaos_runner` 用可复现的故障操作序列持续验证 SPSC 零拷贝 channel 的 ownership、generation fencing、peer liveness、backpressure 和 futex wakeup。它要回答：

- 哪个 seed、哪个 operation index 失败；
- 失败前后 producer/consumer 是否被重启；
- 是否出现 checksum mismatch、lost、duplicate、unexpected message 或 unexpected timeout；
- 恢复探针的尾延迟和进程组 RSS 是否随序列漂移。

它不是随机模糊测试器、性能 benchmark、hard-real-time 证明或“运行若干小时就证明生产可用”的工具。P99 只来自每次操作后的恢复探针，不应与 `fastipc_benchmark` 的 transport workload 数字混用。

## 进程与数据路径

`Runner` 父进程维护两个独立 child：

```text
                 control/report pipe
ChaosRunner  ----------------------------> Producer child
    |                                          |
    |                                          | Loan / Publish
    |                                          v
    |                                  FastIPC shared memory
    |                                          |
    |                                          | Take / Release
    |                                          v
    +------------------------------------ Consumer child
                 control/report pipe
```

匿名 pipe 只承载固定大小的控制命令和 `Status`/counter report。sequence、payload 和 checksum 始终走 public `SharedMemoryTransport` 零拷贝接口，不能因为测试方便绕过 data plane。

父进程拥有有界 fault oracle：

- `SequenceLedger` 要求成功 publish 的 sequence 连续单调；
- deque 只保留当前已 publish、尚未 consume 的 sequence，历史消息不会累积；
- `last_consumed_sequence` 单调标量仍能识别任意久以前的 duplicate；
- payload 前 8 字节是 little-endian sequence，末 8 字节是 FNV-1a checksum，中间字节由 sequence 确定性生成；
- 每项操作结束必须没有 outstanding message，然后执行一次 publish/take 恢复探针。

## Seed 与计划

操作全集固定为：

```text
KillProducer
KillConsumer
RestartProducer
RestartConsumer
SlowConsumer
QueuePressure
Timeout
DelayWakeup
```

`GenerateOperationPlan(seed, count)` 使用 `std::mt19937_64`。每个 8 操作周期先包含全集，再单独 shuffle，因此：

1. 任意完整周期都覆盖全部八类操作；
2. 相同 seed 和 count 得到字节级相同的 operation list；
3. 不同 seed 通常得到不同顺序；
4. seed 只复现操作顺序，不承诺复现 kernel scheduling interleaving。

时长模式用同一 PRNG 流式生成周期，只保留当前 8 个操作，不保存或重建全历史 plan。summary 记录最终 `operation_count`；诊断时可用同一 seed 和该 count 去掉 duration 下限，重放相同操作顺序。

## 八类操作的故障点

| 操作 | 关键步骤 | 成功条件 |
| --- | --- | --- |
| `KillProducer` | producer child `Loan` 后报告 `LoanHeld`，父进程 `SIGKILL`，启动 replacement | abandoned mutable loan 可由死亡身份检查回收；generation 安全；恢复探针通过 |
| `KillConsumer` | publish 后 consumer `Take` 并持有 sample，随后 `SIGKILL` | replacement consumer 至少一次重投递同一 sequence，checksum 正确 |
| `RestartProducer` | graceful stop producer，再打开同一 persistent segment | role/generation 转移后可继续 publish |
| `RestartConsumer` | graceful stop consumer，再打开同一 segment | 新 consumer claim 后可继续 take |
| `SlowConsumer` | 填满 ring，consumer 延迟 release，同时 producer blocking publish | producer 经 `space_epoch` 唤醒；队列最终按 sequence 排空 |
| `QueuePressure` | 填满 ring，分别发送 `Drop` 与 finite `Timeout` | typed status 精确，`dropped_messages` 与 `send_timeouts` 都递增 |
| `Timeout` | 对 live empty channel 执行 finite take | 返回 expected `Timeout`，`receive_timeouts` 递增 |
| `DelayWakeup` | 先发起 blocking take，延迟后 publish | consumer 经 `data_epoch` 唤醒，payload 完整 |

`KillProducer` 和 `KillConsumer` 的死亡点都在真实 RAII handle 未释放时，不是进程空闲时 kill。graceful restart 与 crash restart 分开计数，避免把正常关闭伪装成故障恢复。

## JSONL 合同

输出按行 flush，并可同时 mirror 到 stdout。异常退出前已完成的 operation 仍可诊断。

### environment

包含 schema、run ID、UTC 时间、source revision、build type、compiler、host/kernel/architecture，以及 seed、操作数/时长下限、slot、payload 和 timeout 参数。

### operation

每步包含：

- `operation_index` 与操作名；
- `status` 和 typed status detail；
- 本步恢复探针 `probe_latency_us`；
- 累计 crash/restart/recovery；
- checksum/duplicate/unexpected-timeout/outstanding；
- parent + 两个 actor 的当前 RSS。

### summary

至少包含：

```text
seed
operation_count
crash_count
restart_count
recovery_count
checksum_mismatches
lost_messages
duplicate_messages
unexpected_messages
expected_timeouts
unexpected_timeouts
expected_drops
operation_failures
cleanup_failures
actual_duration_ms
sequence_tracker_mode
maximum_outstanding_messages
retained_plan_operations
probe_samples
retained_probe_samples
baseline_p99_us
final_p99_us
p99_drift_us
rss_start_kib
rss_end_kib
maximum_rss_kib
memory_growth_kib
```

RSS 读取 `/proc/<pid>/statm` 的 resident pages 并汇总父进程和两个 actor。Plan generator 固定保留 8 个操作；Sequence ledger 的存储上限由当前 outstanding 数决定；Latency tracker 只保留最早 N 与最近 N 个探针，`probe_samples` 记录总数，`retained_probe_samples` 最多为 `2N`。这些字段避免 runner 自己用全历史集合制造伪内存增长。RSS 仍只适合发现明显无界增长，不是 allocator leak proof；ASan/LSan 需独立运行。baseline/final P99 分别取探针序列首尾窗口；短跑样本少，因此只能用于回归门槛，不能做容量规划。

## 通过条件与退出码

默认通过条件：

- operation/cleanup failure 为 0；
- checksum mismatch、lost、duplicate、unexpected message、unexpected timeout 为 0；
- 所有 outstanding sequence 在结束前被消费。

可选门槛：

- `--max-memory-growth-kib`；
- `--max-p99-drift-us`。

退出码：

- 0：所有不变量与配置门槛通过；
- 1：正确性不变量、清理或门槛失败；
- 2：参数、输出文件或其他调用错误。

## 构建与短测

```bash
cmake -S projects/fastipc -B projects/fastipc/build-chaos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTIPC_BUILD_TESTS=ON \
  -DFASTIPC_BUILD_CHAOS_RUNNER=ON
cmake --build projects/fastipc/build-chaos
ctest --test-dir projects/fastipc/build-chaos \
  -R 'fastipc\.chaos' --output-on-failure
```

固定 64 操作诊断：

```bash
projects/fastipc/build-chaos/fastipc_chaos_runner \
  --seed 20260821 \
  --operations 64 \
  --payload 4096 \
  --slot-count 8 \
  --peer-timeout-ms 50 \
  --command-timeout-ms 2500 \
  --delay-ms 20 \
  --latency-window 16 \
  --run-id seed-20260821-ops-64 \
  --output projects/fastipc/tests/results/chaos-seed-20260821-ops-64.jsonl
```

## Soak 阶段与证据边界

同一 binary 的时长模式使用 `steady_clock` 下限：

```bash
# 30 分钟
fastipc_chaos_runner --operations 0 --duration-ms 1800000 ...

# 2 小时
fastipc_chaos_runner --operations 0 --duration-ms 7200000 ...

# overnight 示例为 8 小时；报告必须写清所选时长
fastipc_chaos_runner --operations 0 --duration-ms 28800000 ...
```

每一阶段都必须保留：

1. 固定 source revision 的 fresh Release build；
2. 完整 JSONL、SHA-256、seed、精确命令和 host/kernel；
3. summary 与进程退出码；
4. sanitizer 矩阵作为独立证据。

revision `906d0f2d88fb4af504377a4f35f69381ca773366` 已完成一轮真实 30 分钟运行：

| 阶段 | 状态 | 结果 |
| --- | --- | --- |
| 30 分钟 | **PASSED** | 168,954 操作；42,238 crash；84,476 restart/recovery；正确性错误 0；实际 1,800,004.052 ms |
| 2 小时 | **INCOMPLETE** | 未运行，不能从 30 分钟结果外推 |
| overnight（8 小时） | **INCOMPLETE** | 未运行 |
| 24 小时 | **INCOMPLETE** | 只在前述阶段稳定后考虑 |

本轮显式传入 64 MiB RSS 增长门槛和 5 ms P99 漂移门槛。summary 记录 RSS 增长 208 KiB，恢复探针 P99 从 189.500 µs 到 186.844 µs，漂移 -2.656 µs，两个门槛都未超过。完整原始 JSONL 无损压缩为 [证据文件](../tests/results/2026-08-21-chaos-seed-20260821-30min-906d0f2.jsonl.gz)：

```text
原始字节数: 69,633,384
原始 SHA-256: 51e70f47b663a8db01b3972578cb5f6137c9210e6a2495cb9adf8038b5672728
gzip 字节数: 2,305,656
gzip SHA-256: 356107ea33e8c660d7f148988e76ce992c2276696d196da5d754dc5db8767c58
stderr 字节数: 0
```

`gzip -t` 已通过，解压流重新计算得到同一原始 SHA-256；进程自然退出码为 0。逐项 summary、五配置 150/150 矩阵和固定 seed 重放哈希见 [TEST_MATRIX.md](../TEST_MATRIX.md)。

30 分钟通过不代表 2 小时、overnight 或 24 小时已完成，也不等于 target hardware 生产稳定性。24 小时不能代替可复现 seed、逐操作日志和失败定位。

## 已知限制

- 当前 runner 只覆盖 SPSC `SharedMemoryTransport`，不覆盖 MPMC abandoned reservation、UDS 或 fan-out；
- seed 不固定 PID、CPU 调度、page fault 或 futex interleaving；
- RSS 与探针 P99 受 WSL2 和同机负载影响，target Linux 必须重测；
- runner 在第一项 unexpected failure 后停止，以保留最小故障前缀；后续计划不会继续执行；
- threshold 是用户/CI 明确传入的工程门槛，默认不凭短测臆造“合理上限”。
