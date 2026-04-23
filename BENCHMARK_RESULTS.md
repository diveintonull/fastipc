# FastIPC 统一 benchmark 结果

本报告对应统一 benchmark schema v1 的一次真实 Release run。未经改写的原始证据是 [2026-08-21-unified-wsl2-gcc13-91fdeb0.jsonl](benchmarks/results/2026-08-21-unified-wsl2-gcc13-91fdeb0.jsonl)，测量定义见 [benchmark-methodology.md](docs/benchmark-methodology.md)，解释与边界见 [performance-analysis.md](docs/performance-analysis.md)。

## 证据身份

| 字段 | 值 |
| --- | --- |
| UTC | 2026-08-20T23:29:37Z |
| Source revision | `91fdeb0a8fe7b0df4806f5093ec3530c8da54dd6` |
| Raw SHA-256 | `c4ae0630a398384b2ac19bc131d7bb574a244704624ca125cf51efea1deabb90` |
| Run ID | `2026-08-20T23:29:37Z-492832-78476428807561-91fdeb0a8fe7` |
| Build | Release，GNU 13.3.0 |
| Host / kernel | `LAPTOP-NUKUM2JI`，WSL2，`6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel Core Ultra 9 275HX，24 个 online logical CPU |
| Architecture / memory | x86_64，15.33 GiB |
| Protocol | 两个进程、single-outstanding ping-pong；latency 为 RTT |
| Clock / quantile | `steady_clock`；nearest-rank |
| Trial | 每个逻辑 case 3 轮；表格取三轮中位数，raw trial 全部保留 |

原始文件共 150 行：1 条 `environment`、5 条 `baseline_status`、144 条 `result`。144 条结果来自 4 种真实 transport × 2 种访问模式 × 6 种 payload × 3 trial，共 48 个逻辑 case。独立 jq 校验确认：

- 所有记录共享同一 `run_id`，embedded revision 精确匹配；
- 每个 case 恰好 trial 1/2/3；
- `completed_round_trips == iterations`；
- `logical_messages == 2 * iterations`；
- `payload_bytes_transferred == logical_messages * payload_bytes`；
- `P50 <= P95 <= P99 <= P99.9 <= MAX`；
- 每个 result 至少 1,000 个 measured sample；
- iceoryx 没有任何数值 result。

默认 iteration 为 64 B/256 B/1 KiB 各 20,000，4 KiB 为 8,192，64 KiB/1 MiB 各 1,000。warmup 不计入指标。

## Transport-only 中位数

`transport_only` 只写/读前 8 B sequence；表中 MiB/s 是按逻辑 payload 大小换算的 transport capacity，不表示应用真的读写了整块 payload。

| Payload | Copy MiB/s | Copy P99 �s | Zero-copy MiB/s | Zero P99 �s | UDS MiB/s | UDS P99 �s | Pipe MiB/s | Pipe P99 �s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 7.781 | 54.258 | 6.007 | 63.114 | 3.415 | 90.064 | 3.855 | 79.054 |
| 256 B | 31.878 | 53.059 | 24.055 | 77.689 | 14.974 | 86.489 | 17.109 | 70.494 |
| 1 KiB | 103.790 | 54.308 | 78.517 | 77.898 | 57.837 | 83.041 | 61.153 | 83.584 |
| 4 KiB | 361.631 | 60.107 | 350.211 | 65.918 | 226.026 | 87.838 | 274.615 | 75.182 |
| 64 KiB | 3,432.533 | 98.770 | 5,834.547 | 64.405 | 2,482.768 | 114.746 | 2,133.572 | 150.691 |
| 1 MiB | 10,490.071 | 428.291 | 92,353.521 | 58.327 | 2,548.875 | 1,256.767 | 1,371.465 | 2,578.064 |

1 MiB zero-copy 的 92,353.521 MiB/s 是逻辑速率：slot payload 没有被 producer/consumer 全量触碰。三轮分别为 56,641.306、92,353.521、269,373.439 MiB/s，说明 active-spin、futex 与 WSL2 调度形成明显双峰；不能把最高轮当作稳定内存带宽。

## Touch-memory 中位数

`touch_memory` 要求 producer 写完整 payload、consumer 逐字节读并验证完整 payload，更接近“应用实际使用消息内容”的成本边界。

| Payload | Copy MiB/s | Copy P99 �s | Zero-copy MiB/s | Zero P99 �s | UDS MiB/s | UDS P99 �s | Pipe MiB/s | Pipe P99 �s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 5.998 | 67.669 | 5.548 | 68.972 | 4.040 | 70.524 | 3.630 | 89.011 |
| 256 B | 16.798 | 85.446 | 17.784 | 75.180 | 14.092 | 83.939 | 14.677 | 85.927 |
| 1 KiB | 58.141 | 80.160 | 58.120 | 77.471 | 55.095 | 93.718 | 58.091 | 85.091 |
| 4 KiB | 184.123 | 90.875 | 171.901 | 92.560 | 152.317 | 126.455 | 189.369 | 94.756 |
| 64 KiB | 599.063 | 323.747 | 645.660 | 301.708 | 638.981 | 332.048 | 610.322 | 349.501 |
| 1 MiB | 754.574 | 3,403.197 | 832.204 | 2,840.458 | 646.257 | 3,812.628 | 502.663 | 5,131.284 |

在本次 WSL2 run 中：

- 64 B zero-copy 比 copy 低 7.5%，说明小消息的 loan/take ownership 状态机成本高于避免的小复制；
- 1 KiB 两者几乎相同；
- 64 KiB zero-copy 吞吐比 copy 高 7.8%，P99 低 6.8%；
- 1 MiB zero-copy 吞吐比 copy 高 10.3%，P99 低 16.5%；
- 4 KiB pipe 的中位吞吐略高于两种 FastIPC，说明不存在“共享内存对所有 payload 都必胜”的结论；
- 64 KiB 的 UDS 与 zero-copy 很接近，但 UDS 1 MiB 使用 sealed memfd + descriptor transfer，不能称为 pure socket-copy。

## CPU、context switch 与 RSS

1 MiB `touch_memory` 的中位数：

| Transport | CPU % | Context switch | Parent/child peak RSS KiB |
| --- | ---: | ---: | ---: |
| FastIPC copy | 98.590 | 2,045 | 10,360 / 8,260 |
| FastIPC zero-copy | 98.637 | 2,041 | 10,360 / 6,532 |
| UDS sealed memfd | 98.721 | 2,002 | 10,360 / 3,076 |
| Pipe | 85.742 | 61,057 | 10,360 / 2,884 |

pipe 处理 1 MiB 时产生大量 partial-I/O wakeup；其 context switch 明显高于 shared-memory 与 descriptor-assisted UDS。RSS 是进程生命周期峰值，parent 按固定 case 顺序运行后峰值只增不减，所以不能把表中 RSS 当作每条消息的增量内存，也不能跨 row 做精细排名。

## iceoryx 状态

本机 CMake preflight 没有找到 `iceoryx_posh`，当前 revision 也没有实现专用 benchmark adapter。因此 iceoryx 为 **INCOMPLETE / unavailable**，原始文件只有一条带 reason 的 `baseline_status`，没有 throughput、latency 或假的零值。

rigtorp SPSC 不包含跨进程 transport、映射、内核 IPC 或 peer failure，不进入这张公平比较表。

## 验证矩阵

同一最终源码在全新 build directory 中通过：

| 配置 | 结果 | 日志 |
| --- | ---: | --- |
| Debug | 22/22 | [日志](tests/results/2026-08-21-debug-91fdeb0.log) |
| Release | 22/22 | [日志](tests/results/2026-08-21-release-91fdeb0.log) |
| ASan | 22/22 | [日志](tests/results/2026-08-21-asan-91fdeb0.log) |
| UBSan | 22/22 | [日志](tests/results/2026-08-21-ubsan-91fdeb0.log) |
| TSan | 22/22 | [日志](tests/results/2026-08-21-tsan-91fdeb0.log) |

TSan 在该 WSL2 kernel 下使用仓库已记录的 `setarch x86_64 -R` wrapper；不使用它时 runtime 会在测试逻辑前因 shadow-memory mapping 冲突退出。

这些结果只描述所记录主机、固定顺序、未固定 CPU affinity、single-outstanding RTT workload。它们不是 native Linux、目标机器人硬件、NUMA、多 producer 或 saturated streaming 的性能承诺。
