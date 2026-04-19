# FastIPC 上游与衍生实现边界

## 目的

本文件区分 inherited history 与当前 FastIPC engineering。它是作者归属和维护地图，不声称下列每个 idea 在研究意义上都新颖。

## 来源与法律边界

- Primary upstream：[kyr0/libsharedmemory](https://github.com/kyr0/libsharedmemory)
- 固定上游提交：`9e24caaefb28826e99a33be2dd1350725558dd80`
- 本地 subtree 导入提交：`fe161d2`
- 导入 tree：`b18d57de96feeaca5a9048c47e4ed99cb118edba`
- 许可证：MIT；上游版权原文保留于 `LICENSE`
- 额外声明：`test/lest.hpp` 使用 Boost Software License 1.0，并保留 Martin Moene notice

上游 commit 以完整历史导入，没有 squash 或修改日期。工作区 `UPSTREAMS.md` 记录两个衍生项目与 tag namespace。没有 vendor 两个 FastIPC secondary reference 的代码。

## 固定上游提供的内容

导入基线提供紧凑 C++20 仓库、named-memory RAII concept、last-value stream、fixed-capacity queue、example/FFI binding、一个 CTest target、CI/package scaffold。compiled logic 几乎全部位于 `include/libsharedmemory/libsharedmemory.hpp`。

上游 queue 使用 host-native metadata、共享 producer/consumer spin lock 与 shared count；没有 protocol magic/version/total size、initialization election、endpoint identity、generation、heartbeat、futex wait、absolute deadline、typed transport seam、UDS adapter、fault matrix、percentile benchmark、sanitizer matrix 或 profiling evidence。详见 `docs/upstream-analysis.md`。

## 当前构建边界

`CMakeLists.txt` 现在只暴露：

- `FastIPC::fastipc`，由 `src/shared_memory_transport.cpp`、`src/unix_domain_socket_transport.cpp`、`src/status.cpp` 构建；
- 两个当前 behavior/fault test executable；
- 当前 cross-process benchmark executable。

导入的 `include/libsharedmemory`、`example`、`ffi`、`test`、screenshot、legacy changelog 仍保留用于历史审阅，不进入当前 build，也不算 FastIPC behavior。没有单独 destructive-file approval，因此未做广泛删除。

## Keep / Rewrite / Add

### 保留并注明来源

| 项目 | 当前处理 | 证据 |
| --- | --- | --- |
| MIT license 与 copyright | 原文保留 | `LICENSE` |
| 完整上游 development history | 通过 subtree import 保留 | `git log -- projects/fastipc`、import `fe161d2` |
| Named-memory RAII 与 bounded-capacity concept | 作为审计起点 | `docs/upstream-analysis.md` |
| 小型 CMake/CTest 仓库结构 | 重做为当前 target graph | `CMakeLists.txt` |
| Legacy source 与 example | 仅历史用途，不构建 | 当前 `CMakeLists.txt` |

### 重写

| 层面 | 上游 | FastIPC | 证据 |
| --- | --- | --- | --- |
| Public API | concrete stream/queue class | typed `Transport`、`Status`、`Result`、`Deadline`、两种 adapter | `include/fastipc/` |
| Mapping lifecycle | create 前 unlink 与 host-specific wrapper | Linux creator/open validation、mode 0600、size check、initialization election | `src/shared_memory_transport.cpp` |
| Shared layout | 无 identity/version 的 host-native field | magic、2.0 version、endian marker、byte size、generation、endpoint、epoch、isolated cursor，以及逐槽 generation/owner/state tag | `src/shared_memory_layout.hpp` |
| Queue algorithm | lock-serialized side + shared count | SPSC ring；槽位 ownership 由状态 CAS 转移，head/tail 只用精确 expected-cursor CAS 单调发布 | `Loan`、`Publish`、`Take`、`Release` |
| Memory ordering | 部分 atomic，未做 cross-process audit | 逐字段与逐状态 release/acquire 证明、精确游标 CAS，以及 lock-free width assertion | `docs/memory-model.md` |
| Waiting | yield/spin 或立即 false | bounded active spin、epoch futex wait、predicate recheck、monotonic absolute deadline | futex helper、`Send`、`Receive` |
| Backpressure | 立即 boolean failure | `Block`、deadline-bounded `Timeout`、immediate `Drop`、typed counter | `transport.hpp`、`Send` |
| Shutdown lifetime | 无 blocked-operation drain contract | `Close` 标记 closed、wake 两个 epoch、drain active operation lease，最后释放 role/mapping | `OperationLease`、`Impl::Close`、regression |
| Lifecycle error | 小 enum + exception | typed setup/operation status，必要时附 native errno | `status.hpp`、`status.cpp` |
| 仓库入口 | 上游 feature/readme 与 setup Makefile | 当前能力、证据、安全 build command、局限 | `README.md`、`Makefile`、`SECURITY.md` |

### 衍生项目新增

| 能力 | 设计职责 | 证据 |
| --- | --- | --- |
| Role ownership/fencing | PID + Linux start tick + per-claim token；producer restart 推进 generation | `EndpointMetadata`、claim/open path、`OwnsRole` |
| Health/recovery | 专用 heartbeat thread、amortized identity probe、peer-death status、`flock` 下 reclaim | heartbeat/liveness helper、restart test |
| Operation lifetime fencing | active-operation lease 使 concurrent `Close` 等待 blocked call 观察 closure；最后释放 mapping | `shared_memory_transport.cpp`、`LocalCloseWaitsForBlockedOperationBeforeUnmapping` |
| UDS transport | `AF_UNIX` `SOCK_SEQPACKET`；inline frame 与 sealed memfd + `SCM_RIGHTS` | `unix_domain_socket_transport.cpp` |
| Hostile-input validation | wire magic/version/flags/length 与 required memfd seal | UDS receive path、hostile-input test |
| Fault automation | 12 个命名 cross-process shared-memory fault，加 UDS disconnect/corruption | `CMakeLists.txt`、`tests/`、`docs/fault-matrix.md` |
| Benchmark | shared memory、UDS、pipe；64 B–1 MiB；throughput、P50/P95/P99、CPU、context switch、memory | `benchmarks/`、`BENCHMARK_RESULTS.md` |
| Performance engineering | raw non-root perf evidence、rejected probe-only result、bounded-spin 与 heartbeat ownership experiment | `benchmarks/profiling/` |
| Verification matrix | Debug/Release/ASan/UBSan/TSan automation 与固定 revision raw log | workflow、`TEST_MATRIX.md` |
| 设计与代码证据 | memory-model proof、module guide、代码锚点 | `docs/` |

## Commit-level 证据

| Commit | 引入的边界 |
| --- | --- |
| `b3b4ad5` | versioned SPSC shared-memory transport 与 public seam |
| `6faca57` | futex epoch protocol 与 absolute deadline |
| `ab7fcaa` | generation-aware producer restart |
| `4a0f0be` | process death 后 role recovery |
| `edc259d` | heartbeat lease 与 stale-role fencing |
| `0bbdbce` | 自动化 12-case FastIPC fault matrix |
| `6aadf80` | Unix Domain Socket transport 与 large-message descriptor path |
| `2539fb1` | cross-process benchmark harness |
| `f5aed21` 至 `c8c73d6` | baseline profiling 与两项 measured optimization |
| `c7cac63` | 五配置 build/sanitizer verification |
| `31b2107` | active-operation lease 与真实 crash/restart data-flow regression |

在测试版本 `31b2107`，`git diff --shortstat fe161d2:projects/fastipc 31b2107:projects/fastipc` 报告 55 个文件变化、8,740 行新增、635 行删除。它只是评审辅助数据，不是作者占比：生成的测量记录和文档在差异内，保留历史文件在差异外。

## 次要参考

- `vt-tv/lockfree_ipc_ringbuffer`：比较 layout validation、sequence/futex choice、multiprocess test taxonomy。
- `rigtorp/ipc-bench`：比较 benchmark organization 与 transport baseline。

compiled core 没有两者源码。以后若真正 copy/adapt code，而非只参考 idea/behavior，分发前必须加入准确许可证与 attribution。

## 明确不作的声明

FastIPC 不声称 MPSC/MPMC、portable ISO C++ process-shared atomic、public zero-copy loan ownership、authentication/encryption、hard real-time latency、crash-proof mid-instruction recovery、production security review，也不把 WSL2 结果当成 native-hardware performance。这些是明确的未来设计问题，不是暗示存在的能力。
