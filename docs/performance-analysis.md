# FastIPC 性能分析

## 1. 先看结论

本轮证据支持的结论很窄：

1. zero-copy 的收益从中大 payload 开始出现；小消息可能因 ownership 状态机反而更慢；
2. `transport_only` 证明的是“避免 payload copy 后的同步上限”，不是应用吞吐；
3. `touch_memory` 更接近真实消费路径，本机 1 MiB case 中 zero-copy 比 copy 的中位吞吐高 10.3%，P99 RTT 低 16.5%；
4. WSL2 调度足以让 active-spin 路径出现双峰，因此不应只报告最好一轮；
5. Pipe、UDS、copy、zero-copy 在不同 payload 上各有优势，没有单一 transport 普遍获胜；
6. iceoryx 未构建、未适配，所以保持 **INCOMPLETE**，没有比较数字。

原始证据、checksum、完整中位数表见 [BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md)。

## 2. Copy 与 zero-copy 的成本模型

FastIPC copy path 每个方向都要在调用方 buffer 与 shared-memory slot 之间复制：

```text
producer buffer -> request slot
request slot -> child buffer
child buffer -> reply slot
reply slot -> parent buffer
```

zero-copy path 直接对 slot 执行 `Loan/Publish/Take/Release`，避免上述 payload copy，但仍承担：

- slot state transition；
- generation/owner fencing；
- acquire/release/CAS；
- futex epoch 与 bounded active-spin；
- RAII handle 建立与归还；
- ping-pong 的 process scheduling。

因此 64 B–1 KiB 时，省下的复制很小，ownership bookkeeping 可以主导成本。本轮 `transport_only` 中 64 B、256 B、1 KiB 的 zero-copy 吞吐分别只有 copy 的 77.2%、75.5%、75.6%；到 4 KiB 才接近，64 KiB 与 1 MiB 才明显拉开。

## 3. 为什么必须同时看两种访问模式

### Transport-only

producer 只写 sequence，consumer 只读 sequence。1 MiB zero-copy 没有实际搬运 1 MiB，因此 92,353.521 MiB/s 只是：

```text
logical_messages * declared_payload_bytes / wall_time
```

它回答“如果业务已经能原地生产、原地消费，transport ownership 能有多薄”，不能回答 DRAM 带宽或序列化吞吐。

### Touch-memory

producer 写满，consumer 扫描并验证全部 payload。此时应用内存访问开始支配大消息：

| Payload | Zero/copy 吞吐比 | Zero/copy P99 比 |
| ---: | ---: | ---: |
| 64 B | 0.925� | 1.019� |
| 256 B | 1.059� | 0.880� |
| 1 KiB | 1.000� | 0.966� |
| 4 KiB | 0.934� | 1.019� |
| 64 KiB | 1.078� | 0.932� |
| 1 MiB | 1.103� | 0.835� |

数值不是单调曲线，说明 scheduler、cache state 和扫描循环噪声仍显著。可以说“本机 64 KiB/1 MiB 显示收益”，不能说“4 KiB 是普适 break-even”。

## 4. Active-spin 与 futex 双峰

最明显的例子是 1 MiB zero-copy `transport_only`：

| Trial | MiB/s | P50 RTT �s | P99 RTT �s | CPU % | Context switch |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 56,641.306 | 33.969 | 69.030 | 60.111 | 2,000 |
| 2 | 92,353.521 | 22.630 | 58.327 | 71.902 | 1,591 |
| 3 | 269,373.439 | 2.130 | 42.634 | 102.335 | 440 |

trial 3 大量 round trip 在 active-spin 窗口内完成，避免 futex sleep/wakeup；前两轮更多进入 sleep。吞吐 max/min spread 达 375.6%。这不是数据损坏：三轮 exact-count、payload validation 与状态均通过，而是调度状态不同。

因此框架保留每轮 raw result，并在报告中用中位数。下一阶段要得到 target-grade 结论，应加入并记录：

- parent/child 分离 CPU affinity；
- CPU governor、frequency 与 thermal state；
- case 顺序随机化或 Latin-square；
- native Linux isolation / `isolcpus`；
- 更多 trial 与 bootstrap confidence interval；
- active-spin budget sweep。

## 5. Baseline 的公平边界

### Pipe

两条 nonblocking pipe，每个方向一条。1 MiB 必须多次 read/write，`touch_memory` 中位 context switch 为 61,057，远高于其他三种路径约 2,000。它是有效 kernel-copy baseline，但不适合作固定 frame 的零系统调用目标。

### Unix Domain Socket

64 KiB 内使用 `SOCK_SEQPACKET` inline record；1 MiB 使用 sealed memfd + `SCM_RIGHTS`。后者已经借助 shared memory，结果标记为 `seqpacket_sealed_memfd`，不能写成“纯 UDS copy”。

### iceoryx

仅有 package 还不够，必须有相同 ping-pong、访问模式、payload validation 与 resource accounting 的 adapter。本机 package 不存在，adapter 也未实现，所以不输出数值。缺失 baseline 的 exit code 3 与 machine-readable status 已由 smoke test 覆盖。

### rigtorp SPSC

线程/队列 microbenchmark 不承担进程调度、mapping、kernel IPC、liveness 或 descriptor transfer。把它放进同一性能排名会改变问题定义，因此只可作 queue algorithm 参考。

## 6. CPU、context switch 与内存

CPU 是 parent/child measured-window user+system time 除以 parent wall time，所以两个进程并行时可超过 100%。它不是单核占用率。

context switch 对解释状态切换很有帮助，但不能单独代表效率：active-spin 会减少 switch、增加 CPU；futex 会降低空转、增加 wakeup latency。应同时看 CPU、context switch 与 tail latency。

`ru_maxrss` 是进程生命周期峰值。runner 固定按 transport 顺序执行，parent peak 会继承前面 case 的高水位；它只能排查数量级异常，不能用来证明每消息“节省了多少内存”。

## 7. 统计限制

本轮每个 case 只有 3 trial；中位数能避免单个极端值直接成为结论，但不能构成窄置信区间。默认每轮至少 1,000 measured sample，使 nearest-rank P99.9 不再因样本少于 1,000 直接退化为 MAX，但一个 trial 的 P99.9 仍只由极少数尾部样本决定。

此外：

- workload 是 single-outstanding RTT，不是 saturated throughput；
- 不是 one-way latency；
- 没有固定 CPU affinity；
- case 顺序未随机化；
- WSL2 不是目标机器人 native Linux；
- 没有 NUMA、contention、MPMC 或多 client；
- `touch_memory` 仍不包含业务序列化与算法。

所以这些数字适合验证 benchmark harness、寻找 break-even 与提出下一轮假设，不适合作 SLA 或营销数字。

## 8. 下一轮实验

优先顺序：

1. native Linux 上固定 parent/child 独立 CPU，记录 affinity/governor；
2. 将 trial 提升到至少 10，并随机化 case 顺序；
3. 对 4 KiB–1 MiB 做 active-spin budget sweep；
4. 增加 saturated streaming 与多 outstanding，但与 RTT 表分开；
5. 实现并验证 iceoryx adapter 后再加入相同 schema；
6. MPMC 完成后单列 producer/consumer contention matrix；
7. 用 `perf stat/record` 对 copy、scan、futex、scheduler 热点做归因。

任何下一轮都应继续保存原始 JSONL，不只保存汇总表。
