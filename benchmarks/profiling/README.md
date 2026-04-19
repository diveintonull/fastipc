# Shared-memory profiling 与优化实验

本目录保留要求的 `perf stat`、`perf record`、`perf report` 原始证据。以下性能声明只适用于所记录 WSL2 host 与 workload。
提交身份改写导致当前提交 ID 与原始 profiling 文件名中的旧 ID 不同；对照见 [提交身份改写映射](../../../../COMMIT_IDENTITY_MAP.md)。原始测量文件未改写。

## Profiling 环境

| 字段 | 值 |
| --- | --- |
| 日期 | 2026-08-20 |
| Baseline source | `8b8158f8dcd1` |
| Compiler | GNU 13.3.0 |
| Profile build | `-O2 -g -fno-omit-frame-pointer` |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| perf | Ubuntu `linux-tools-6.8.0-138` 的 6.8.12，无 root 解压到 `/tmp` |
| `perf_event_paranoid` | 2 |
| Workload | shared memory，64 B，200,000 measured RTT，1,000 warmup RTT |

WSL2 只暴露 software event；hardware `cycles`、`instructions`、`cache-references`、`cache-misses` 报 `not supported`。`perf stat` 还强制 user-only event，因此其 context-switch 为 0；context-switch 证据使用 benchmark 的 parent+child `getrusage` delta。这些限制必须原样报告，不能估算替代值。

## 复现命令

profiling build：

```bash
cmake -S projects/fastipc \
  -B /tmp/fastipc-profile-baseline-6da1a9e \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" \
  -DFASTIPC_BUILD_TESTS=OFF \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build /tmp/fastipc-profile-baseline-6da1a9e \
  --target fastipc_benchmark
```

下列 `PERF` 代表解压的 6.8.12 binary，并在 `LD_LIBRARY_PATH` 中加入解压的 `libtraceevent.so.1`：

```bash
PERF stat -r 5 -x, \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o baseline-6da1a9e-stat.csv -- \
  fastipc_benchmark --transport=shared_memory --payload=64 \
  --iterations=200000 --warmup=1000

PERF record -e cpu-clock -F 499 -g \
  -o baseline-6da1a9e.data -- \
  fastipc_benchmark --transport=shared_memory --payload=64 \
  --iterations=200000 --warmup=1000

PERF report --stdio --no-children --sort symbol \
  --percent-limit 0.5 --call-graph none \
  -i baseline-6da1a9e.data
```

host-specific `perf.data` 未提交；提交了 rendered report、record run 的 benchmark output、五次 `stat` repetition 与 CSV。text report 只去掉 table padding，numeric/symbol content 未变。

## Baseline 证据

文件：

- [baseline-6da1a9e-stat.csv](baseline-6da1a9e-stat.csv)
- [baseline-6da1a9e-runs.jsonl](baseline-6da1a9e-runs.jsonl)
- [baseline-6da1a9e-record-run.jsonl](baseline-6da1a9e-record-run.jsonl)
- [baseline-6da1a9e-report.txt](baseline-6da1a9e-report.txt)

median-throughput repetition：

| Metric | Baseline |
| --- | ---: |
| Logical messages/s | 153,825.684 |
| Wall time | 2,600.346 ms |
| P50 RTT | 4.074 us |
| P95 RTT | 34.345 us |
| P99 RTT | 54.549 us |
| Summed CPU time | 2,799.776 ms |
| Voluntary context switch | 161,403 |

`perf stat -r 5`：task-clock 2,654.07 ms，variation 0.43%；page fault 635，variation 0.04%。

flat 415-sample report：

| Symbol | Self overhead |
| --- | ---: |
| `SharedMemoryTransport::Send` | 10.12% |
| `/proc/<pid>/stat` parsing 的 string extraction | 8.43% |
| `SharedMemoryTransport::Receive` | 7.71% |
| `ProcessStartTicks` | 5.54% |
| `__memmove_avx_unaligned_erms` | 5.30% |
| process-stat parsing 的 iostream sentry | 4.82% |
| `__vdso_clock_gettime` | 4.58% |
| `syscall` | 4.58% |

诊断用 call graph 表明 string/iostream work 位于 `ProcessStartTicks -> InspectEndpoint -> PeerStatus -> Receive`。clock sample 同时包含 benchmark timing 与 transport Send/Receive。

## 实验假设

### 实验 1：amortize process identity probe

queue empty/full path 在每次 futex wait 前重新打开并解析 `/proc/<pid>/stat`，用于防 PID reuse；正确但在 heartbeat lease 仍 fresh 时过于昂贵。

目标改动：setup、lease expiry 后、bounded probe interval 时仍做完整 PID/start-tick check；futex wait 也受该 interval 限制，不能让 infinite caller deadline 隐藏 crash。这样保留 PID-reuse fencing 与 bounded crash detection，同时移除大多数 message 的 filesystem/iostream work。

### 实验 2：移除 message path 重复 heartbeat timestamp

每次成功 Send/Receive 都调用 `steady_clock::now` 并写 heartbeat cache line，但 dedicated heartbeat thread 已负责刷新 lease。目标是让该 thread 独占 heartbeat timestamp，同时保留 per-message `operation_sequence` 作为 progress signal。

两项变化都必须先通过完整 fault/sanitizer matrix，测量才有效。

## 实验 1 结果：identity-probe amortization + bounded spin

artifact 同时保留第一次改动与 evidence-driven correction：

- `experiment1-probe-only-e609fa8-*`：amortize full identity read，没有 active spin。
- `experiment1-bounded-spin-f24f832-*`：相同 liveness design，加 public bounded 256-relax default。

相同 five-run workload 的 median：

| Metric | Baseline `8b8158f` | Probe only `4f315af` | Bounded spin `c3f053f` |
| --- | ---: | ---: | ---: |
| Logical messages/s | 153,825.684 | 84,453.358 | 209,670.456 |
| Wall time | 2,600.346 ms | 4,736.342 ms | 1,907.756 ms |
| P50 RTT | 4.074 us | 21.902 us | 0.922 us |
| P95 RTT | 34.345 us | 34.013 us | 32.364 us |
| P99 RTT | 54.549 us | 54.362 us | 49.771 us |
| Summed CPU time | 2,799.776 ms | 2,239.723 ms | 1,445.745 ms |
| Voluntary context switch | 161,403 | 399,942 | 139,654 |

probe-only 确实移除了 filesystem/iostream work，task-clock 从 2,654.07 降到 1,659.18 ms；但过早进入 futex sleep，context switch +147.791%，throughput -45.098%。flat report 不再有 `ProcessStartTicks`/iostream hot symbol，但 `syscall` 升至 user CPU sample 的 23.90%，`FutexWait` 为 5.88%。这个中间结果被明确拒绝，没有隐藏。

加入 256-iteration CPU-relax 后，relative baseline：

- logical messages/s +36.304%；
- summed CPU time -48.362%；
- voluntary context switch -13.475%；
- P50 RTT -77.369%；
- 本 run 的 P95/P99 也更低。

task-clock 1,259.58 ms。flat report 不再含 process-stat parsing symbol；`syscall` 12.03%，`FutexWait` 0.57%。`Receive` 占 62.18%，因为有意的 active-spin loop 现在承担 user-space waiting；不能单凭 sample percentage 判定 regression，接受依据是 wall/CPU/latency/context-switch。

最终 policy 接受可调 latency-vs-CPU trade-off：0 禁用 active spin；256 是 measured default；大于 65,536 拒绝。implementation commit 前，完整 Debug suite 通过 15/15。

## 实验 2 结果：heartbeat-thread timestamp ownership

artifact：

- [experiment2-heartbeat-thread-866d32b-stat.csv](experiment2-heartbeat-thread-866d32b-stat.csv)
- [experiment2-heartbeat-thread-866d32b-runs.jsonl](experiment2-heartbeat-thread-866d32b-runs.jsonl)
- [experiment2-heartbeat-thread-866d32b-record-run.jsonl](experiment2-heartbeat-thread-866d32b-record-run.jsonl)
- [experiment2-heartbeat-thread-866d32b-report.txt](experiment2-heartbeat-thread-866d32b-report.txt)

before 为已接受 experiment-1 revision `c3f053f`；after 为 `c8c73d6`。build flag、perf command、payload、warmup、五次 repetition 相同。

| Metric | Before `c3f053f` | After `c8c73d6` | Change |
| --- | ---: | ---: | ---: |
| Logical messages/s | 209,670.456 | 216,994.748 | +3.493% |
| Wall time | 1,907.756 ms | 1,843.363 ms | -3.375% |
| P50 RTT | 0.922 us | 0.895 us | -2.928% |
| P95 RTT | 32.364 us | 27.601 us | -14.717% |
| P99 RTT | 49.771 us | 45.062 us | -9.461% |
| Summed CPU time | 1,445.745 ms | 1,434.544 ms | -0.775% |
| Voluntary context switch | 139,654 | 142,183 | +1.811% |

task-clock 从 1,259.58 降到 1,239.93 ms（-1.560%）；flat report 中 `__vdso_clock_gettime` 从 5.16% 降到 4.31%。sampling percentage noisy 且不可相加，接受依据仍是 wall/latency/CPU。

context-switch +1.811% 被如实报告；它落在 WSL2 run-to-run variation 内，同时三项 latency quantile、throughput、wall time、task-clock、summed CPU 都按预期改善。保留此改动还因为 lease timestamp 由一个 component 明确拥有。

相对原始 `8b8158f` baseline，最终 `c8c73d6` median：messages/s +41.065%，summed CPU -48.762%，voluntary context switch -11.908%，P50/P95/P99 分别 -78.031%/-19.636%/-17.392%。这些只适用于该 64 B WSL2 ping-pong experiment。

实验 2 后完整 Debug suite 通过 15/15；Release/sanitizer matrix 在项目完成前另行记录。
