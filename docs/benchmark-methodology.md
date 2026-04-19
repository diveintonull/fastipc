# FastIPC benchmark 方法

## 目的

benchmark 在一个可复现 cross-process protocol 下，比较两种 FastIPC transport 与 Linux pipe baseline。它报告 measurement，不声称某种 transport 在所有 workload 下都更快或都适用。

## 必测矩阵

| Transport | 模式 |
| --- | --- |
| `shared_memory` | 两条单向 SPSC ring，使用 futex epoch wakeup |
| `unix_domain_socket` | `SOCK_SEQPACKET`；64 KiB 内 inline，以上使用 sealed `memfd` + `SCM_RIGHTS` |
| `pipe` | 两条 nonblocking pipe，每个方向一条 |

payload：64 B、256 B、1 KiB、4 KiB、64 KiB、1 MiB。

## Protocol

每个 case 恰好 fork 一个 child。parent 发送一条 payload，等待 child echo，再发下一条。前 8 B 是 sequence；最终 payload 做 byte-for-byte comparison。

这是 single-outstanding ping-pong：

1. 测量 RTT，不是 one-way latency；
2. 覆盖两个方向的 process wakeup、transport send/receive、echo；
3. 不测 saturated multi-producer stream；
4. throughput 从完成 RTT 推导；一个 RTT 算两条 logical message、传两份 payload。

shared memory 使用分离的 request/reply channel，因为每条 ring 都是 SPSC + unidirectional。socket 为 full duplex；pipe baseline 每方向一条 kernel pipe。

## Iteration 与 warmup

默认 measured iteration 目标约 64 MiB bidirectional payload：

```text
iterations = clamp(
    64 MiB / (2 * payload_bytes),
    100,
    20,000)
```

warmup 不进入任何 metric：

```text
warmup = clamp(iterations / 20, 5, 100)
```

command line 可覆盖二者。`--self-test` 在 64 B/1 MiB 只做 3 次 measured + 1 次 warmup，用于校验 harness/schema，不代表性能。

## Timing 与 quantile

parent 用 `std::chrono::steady_clock` 记录每个 RTT。P50/P95/P99 对 measured iteration 使用 nearest-rank。wall time 只包围 measured parent loop。

```text
round_trips_per_second = iterations / wall_seconds
messages_per_second    = 2 * iterations / wall_seconds
payload_mib_per_second = 2 * iterations * payload_bytes
                         / (MiB * wall_seconds)
```

## CPU、context switch、memory

parent/child 在 measured loop 前后调用 `getrusage(RUSAGE_SELF)`，runner 汇总 delta：

- user CPU time；
- system CPU time；
- voluntary context switch；
- involuntary context switch。

CPU utilization = summed CPU time / parent wall time；两个 process 并行时可超过 100%。

`parent_peak_rss_kib` 与 `child_peak_rss_kib` 是 Linux `ru_maxrss`，为 process-lifetime peak，不是 measured-window delta；它还包含 runner、allocator、transport setup。只能当 coarse memory signal，不能视为 precise per-message allocation。

## UDS 大消息解释

不超过 64 KiB 的 message 在一个 `SOCK_SEQPACKET` record 内 inline 发送。更大 message（含 1 MiB case）复制到 sealed anonymous `memfd`；通过 `SCM_RIGHTS` 传 descriptor；receiver 校验 type、exact size、required seal 后 mapping。

结果分别标记 `seqpacket_inline` 与 `seqpacket_sealed_memfd`。后者是 descriptor-assisted shared memory，不能表述为 pure socket-copy throughput。

## Environment record

第一条 JSONL 记录 UTC、hostname、kernel、architecture、CPU model、online logical CPU count、page size、total memory、compiler、build type，以及 CMake configure 时嵌入的 source revision；之后每行是一条 result。

为了得到可比较证据：

1. benchmark commit 存在后再配置 Release build，让 embedded revision 准确；
2. 在尽量 idle 的机器运行 full matrix；
3. 保存 raw JSONL，不只复制 terminal summary；
4. 做性能结论时重复 run；
5. 记录 virtualization、power management、CPU affinity 条件。

WSL2 结果只证明所记录 host 的行为，不替代 native Linux/target hardware。

## 命令

```bash
cmake -S projects/fastipc -B projects/fastipc/build-release \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFASTIPC_BUILD_TESTS=ON -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build projects/fastipc/build-release --target fastipc_benchmark
projects/fastipc/build-release/fastipc_benchmark
```

用 `--help` 查看 transport、payload、iteration、warmup filter。stdout 为 JSONL；diagnostic 与 typed failure 输出到 stderr。
