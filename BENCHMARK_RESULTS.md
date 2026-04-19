# FastIPC benchmark 结果

本报告来自一次真实 Release run。未经改动的证据是 [2026-08-20-wsl2-gcc13-c5cb58a.jsonl](benchmarks/results/2026-08-20-wsl2-gcc13-c5cb58a.jsonl)；测量定义与限制见 [benchmark-methodology.md](docs/benchmark-methodology.md)。
提交身份改写导致当前提交 ID 与原始证据文件名中的旧 ID 不同；对照见 [提交身份改写映射](../../COMMIT_IDENTITY_MAP.md)。原始 JSONL 未改写。

## 证据身份

| 字段 | 值 |
| --- | --- |
| UTC timestamp | 2026-08-20T06:14:40Z |
| Source revision | `2539fb1efd56` |
| Build | Release，GNU 13.3.0 |
| Host | `LAPTOP-NUKUM2JI`，WSL2 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel Core Ultra 9 275HX，24 个 online logical CPU |
| Architecture | x86_64 |
| Memory | `/proc/meminfo` 报告 15.33 GiB |
| Clock / quantile | `steady_clock`；nearest-rank |
| Protocol | cross-process、single-outstanding ping-pong；latency 为 RTT |

数据包含 1 条 environment record 与 18 条 result record：三种 transport × 六种 payload。

## Shared memory：`spsc_ring_futex`

| Payload | Iterations | Msg/s | MiB/s | P50 us | P95 us | P99 us | CPU % | Ctx V/I | RSS KiB P/C |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 20,000 | 136,340.327 | 8.322 | 20.278 | 32.844 | 49.784 | 100.062 | 20,679 / 13 | 3,840 / 2,880 |
| 256 B | 20,000 | 134,551.909 | 32.850 | 21.339 | 28.519 | 43.531 | 99.714 | 21,591 / 1 | 3,840 / 2,304 |
| 1 KiB | 20,000 | 141,888.891 | 138.563 | 13.976 | 27.801 | 42.341 | 103.242 | 19,897 / 0 | 3,840 / 2,504 |
| 4 KiB | 8,192 | 134,734.303 | 526.306 | 6.684 | 33.092 | 40.673 | 109.519 | 7,371 / 0 | 3,840 / 2,496 |
| 64 KiB | 512 | 54,554.168 | 3,409.635 | 35.805 | 47.037 | 59.913 | 98.927 | 1,007 / 0 | 4,032 / 2,688 |
| 1 MiB | 100 | 11,790.426 | 11,790.426 | 166.157 | 189.523 | 222.607 | 106.863 | 199 / 0 | 9,784 / 7,296 |

## Unix Domain Socket

64 B–64 KiB 使用 `seqpacket_inline`；1 MiB 使用 `seqpacket_sealed_memfd`，因此是 descriptor-assisted shared memory，不是 pure socket-copy throughput。

| Payload | Iterations | Msg/s | MiB/s | P50 us | P95 us | P99 us | CPU % | Ctx V/I | RSS KiB P/C |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 20,000 | 71,405.894 | 4.358 | 24.111 | 39.023 | 56.032 | 52.735 | 39,982 / 0 | 9,784 / 1,344 |
| 256 B | 20,000 | 70,167.908 | 17.131 | 23.597 | 49.915 | 74.412 | 51.391 | 39,985 / 0 | 9,784 / 1,344 |
| 1 KiB | 20,000 | 72,891.012 | 71.183 | 24.001 | 39.910 | 56.880 | 53.435 | 39,986 / 0 | 9,784 / 1,344 |
| 4 KiB | 8,192 | 69,903.345 | 273.060 | 25.292 | 40.379 | 53.898 | 54.557 | 16,384 / 0 | 9,784 / 1,344 |
| 64 KiB | 512 | 52,200.913 | 3,262.557 | 36.363 | 45.711 | 77.293 | 68.718 | 1,024 / 0 | 9,784 / 1,344 |
| 1 MiB | 100 | 3,359.819 | 3,359.819 | 551.098 | 798.621 | 900.933 | 94.663 | 200 / 0 | 9,784 / 2,112 |

## Pipe：`dual_nonblocking_pipe`

| Payload | Iterations | Msg/s | MiB/s | P50 us | P95 us | P99 us | CPU % | Ctx V/I | RSS KiB P/C |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 20,000 | 78,471.689 | 4.790 | 21.261 | 39.609 | 56.080 | 44.184 | 39,987 / 0 | 9,784 / 968 |
| 256 B | 20,000 | 79,296.616 | 19.360 | 21.042 | 37.109 | 57.260 | 44.814 | 39,974 / 1 | 9,784 / 1,152 |
| 1 KiB | 20,000 | 89,900.934 | 87.794 | 19.230 | 32.971 | 46.555 | 49.132 | 39,978 / 0 | 9,784 / 1,152 |
| 4 KiB | 8,192 | 95,579.600 | 373.358 | 19.111 | 30.535 | 49.164 | 50.668 | 16,380 / 0 | 9,784 / 1,152 |
| 64 KiB | 512 | 49,511.514 | 3,094.470 | 37.028 | 53.365 | 98.073 | 77.753 | 1,024 / 0 | 9,784 / 1,152 |
| 1 MiB | 100 | 2,047.105 | 2,047.105 | 962.418 | 1,163.845 | 1,216.347 | 70.736 | 6,138 / 0 | 9,784 / 1,920 |

## 本次 run 说明什么

- 64 B–4 KiB：shared memory 约 134k–142k logical msg/s，socket 约 70k–73k，pipe 约 78k–96k。
- 64 KiB：同步协议下三条路径约 3.09–3.41 GiB/s。
- 1 MiB：shared memory 为 11,790 MiB/s、P50 RTT 166 us；sealed memfd socket 为 3,360 MiB/s、551 us；pipe 为 2,047 MiB/s、962 us。
- small-message socket/pipe 每 RTT 接近两次 voluntary context switch；shared memory 多数 row 接近一次。
- summed CPU 可超过 100%，因为包含两个 process。

这些只是一次 WSL2 run 的 observation，不是 production claim 或 universal ranking。scheduler placement、CPU frequency、native Linux、NUMA topology、contention、multi-outstanding workload 需要独立实验。
