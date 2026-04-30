# FastIPC

FastIPC 是 Linux C++20 IPC library，基于 `kyr0/libsharedmemory` 深度派生并明确标注来源。当前 core 提供 versioned SPSC zero-copy channel、独立的 bounded MPMC copy channel、带 absolute deadline 的 futex blocking、peer liveness/restart fencing、显式 backpressure，以及 Unix Domain Socket transport。

## 能力

- Linux POSIX shared-memory mapping，带 creator/open validation，默认 mode 0600；
- versioned、endian-marked layout，包含 total size、initialization state、generation 与 chunk lifecycle；
- cache-line 隔离的 SPSC head/tail cursor 与 fixed-capacity slot；
- 独立的 bounded MPMC：全局 enqueue/dequeue CAS reservation、per-slot sequence、跨线程/跨进程并发；
- MPMC 正常执行时提供 FIFO reservation 与恰好一次竞争消费；abandoned reservation 恢复明确标为 `INCOMPLETE`；
- `Loan/Publish/Take/Release` 零拷贝 ownership API，move-only RAII handle；
- warmed 成功路径无逐消息普通堆分配，copy API 复用同一 lifecycle；
- generation、owner token 与 slot state 的 acquire/release/CAS 审计；
- futex epoch、sleep 前 recheck、monotonic absolute deadline、bounded active spin；
- PID、Linux process-start tick、role token、heartbeat lease、generation fencing；
- producer/consumer crash detection、stale segment recovery、restart flow；
- Block、Timeout、Drop backpressure，返回 typed `Status` 与 counter；
- `Transport` API，包含 `SharedMemoryTransport`、`UnixDomainSocketTransport`；
- UDS `SOCK_SEQPACKET`：64 KiB 内 inline frame；更大 payload 使用 sealed-memfd descriptor transfer；
- 64 B 至 1 MiB 的 cross-process benchmark matrix，区分 copy/zero-copy 与 transport-only/touch-memory；
- Pipe、UDS、FastIPC Copy、FastIPC Zero-copy 四种真实 baseline；iceoryx 缺失时输出机器可读 unavailable，不伪造结果；
- schema v1 JSONL，包含 run/case/trial identity、精确消息/字节计数、P50/P95/P99/P99.9/MAX、CPU、context switch、RSS；
- 自动 Debug、Release、ASan、UBSan、TSan 覆盖。
- 可复现的 seeded `ChaosRunner`：八类故障按 seed 洗牌，producer/consumer 为独立进程，每步输出增量 JSONL；
- 每次故障后执行恢复探针，并核对 checksum、lost、duplicate、unexpected timeout、RSS 增长与 P99 漂移。

## 架构

```text
Application
    |
    +-- SharedMemoryTransport
    |      +-- Copy seam { Send, Receive }
    |      +-- Ownership seam
    |      |      { Loan, Publish, Take, Release }
    |      +-- versioned mapped chunk pool
    |      +-- bounded SPSC ring
    |      +-- per-slot state / generation / owner identity
    |      +-- active spin -> futex epoch wait
    |      +-- generation / role / heartbeat control plane
    |
    +-- MpmcSharedMemoryTransport
    |      +-- Copy seam { Send, Receive }
    |      +-- bounded power-of-two ring
    |      +-- enqueue/dequeue position CAS
    |      +-- per-slot sequence acquire/release
    |      +-- active spin -> futex epoch wait
    |
    +-- UnixDomainSocketTransport
           +-- AF_UNIX SOCK_SEQPACKET
           +-- inline frame or sealed memfd + SCM_RIGHTS
```

SPSC shared memory 分为两条 ownership plane：

- data plane：一个 producer 独占 `head` 与 slot write；一个 consumer 独占 `tail` 与 slot read；
- control plane：role claim、generation、PID/start-ticks identity、heartbeat、cleanup。

heartbeat `jthread` 是 lease timestamp 的唯一 writer；成功 message operation 只增加独立 progress sequence。

MPMC 使用另一套 magic/layout，没有 endpoint role 与 Loan handle；任何成功 `Open` 的 participant 都可并发 Send/Receive。两套布局与算法刻意隔离，避免 MPMC CAS 进入已验证的 SPSC 热路径。

## 构建与测试

在本目录执行：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTIPC_BUILD_TESTS=ON \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

默认 build 生成：

- `FastIPC::fastipc`：static library target；
- `fastipc_tests`：shared-memory behavior/fault executable；
- `fastipc_mpmc_tests`：MPMC 单元、线程、跨进程与 abandoned-reservation executable；
- `fastipc_uds_tests`：Unix-socket behavior/hostile-input executable；
- `fastipc_benchmark`：跨进程 transport JSONL benchmark runner；
- `fastipc_mpmc_benchmark`：1P1C/2P2C/4P4C contention JSONL runner。
- `fastipc_chaos_runner`：可配置 seed、操作数、最短时长和阈值的跨进程故障编排器；
- `fastipc_chaos_tests`：确定性计划和八操作精确汇总断言；
- `fastipc_chaos_support`：只供测试工具使用的内部 static library，不是 public transport API。

## Shared-memory 示例

```cpp
#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

fastipc::ChannelConfig config;
config.name = "sensor_frames";
config.slot_count = 64;
config.max_message_size = 4096;
config.unlink_on_owner_close = true;

auto producer_result =
    fastipc::SharedMemoryTransport::CreateProducer(config);
auto consumer_result =
    fastipc::SharedMemoryTransport::OpenConsumer(config);
if (!producer_result || !consumer_result) {
  // Inspect result.status(); setup failures are typed.
  return;
}

auto producer = std::move(producer_result).take_value();
auto consumer = std::move(consumer_result).take_value();

const std::array<std::byte, 3> payload{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
auto sent = producer->Send(
    payload,
    fastipc::SendOptions{
        fastipc::BackpressurePolicy::Block,
        fastipc::Deadline::After(100ms)});

std::array<std::byte, 4096> destination{};
auto received =
    consumer->Receive(destination, fastipc::Deadline::After(100ms));
```

零拷贝写法：

```cpp
auto loan = producer->Loan(
    4096,
    {fastipc::BackpressurePolicy::Block,
     fastipc::Deadline::After(100ms)});
if (loan) {
  FillInPlace(loan.value().Data());
  loan.value().Publish();
}

auto sample = consumer->Take(fastipc::Deadline::After(100ms));
if (sample) {
  ConsumeInPlace(sample.value().Data());
  sample.value().Release();
}
```

producer 与 consumer 通常位于不同 process；这里放在一起仅为展示 public seam。

## MPMC 示例

```cpp
#include <fastipc/mpmc_shared_memory_transport.hpp>

fastipc::MpmcChannelConfig config;
config.name = "worker_queue";
config.capacity = 1024;  // 必须是至少 2 的 2 次幂。
config.max_message_size = 4096;
config.unlink_on_owner_close = true;

auto owner_result =
    fastipc::MpmcSharedMemoryTransport::Create(config);
auto peer_result =
    fastipc::MpmcSharedMemoryTransport::Open(config);
if (!owner_result || !peer_result) {
  return;
}

auto owner = std::move(owner_result).take_value();
auto peer = std::move(peer_result).take_value();

owner->Send(
    payload,
    {fastipc::BackpressurePolicy::Block,
     fastipc::Deadline::After(100ms)});
peer->Receive(
    destination, fastipc::Deadline::After(100ms));
```

`Create` 只做独占初始化；creator 与 opener 都能并发生产或消费。MPMC 当前是 copy-in/copy-out，不提供 Loan API。destination 太小时，已经 reserve 的消息会被释放并计为 dropped，再返回 `BufferTooSmall`，以免全局 dequeue cursor 永久停住。

## Backpressure 与 failure surface

`SendOptions` policy：

| Policy | queue 满时行为 |
| --- | --- |
| `Block` | bounded active spin，随后 futex sleep，直至有空间、close、peer failure 或 deadline |
| `Timeout` | 使用相同 bounded wait；到 deadline 返回 `Timeout` |
| `Drop` | 立即返回 `Dropped`，不做 unbounded allocation |

其他 typed outcome：`Closed`、`PeerUnavailable`、`PeerDead`、`StaleGeneration`、`RoleConflict`、`LayoutMismatch`、`MessageTooLarge`、`BufferTooSmall`、`CorruptData`。

## 证据

- [零拷贝设计](docs/zero-copy-design.md)
- [chunk 生命周期与恢复矩阵](docs/chunk-lifecycle.md)
- [MPMC 算法、证明边界与故障模型](docs/mpmc-design.md)
- [Copy 与 Zero-copy 实测结果](ZERO_COPY_BENCHMARK_RESULTS.md)
- [统一 Benchmark 设计与 JSONL 合同](docs/benchmark.md)
- [Benchmark 方法](docs/benchmark-methodology.md)
- [原始 benchmark 与完整结果](BENCHMARK_RESULTS.md)
- [性能分析与统计边界](docs/performance-analysis.md)
- [两轮 perf 实验与原始 stat/record/report](benchmarks/profiling/README.md)
- [五种配置测试矩阵与原始 CTest 日志](TEST_MATRIX.md)
- [故障矩阵](docs/fault-matrix.md)
- [Seeded Chaos / Soak 设计、复现与证据边界](docs/chaos-testing.md)
- [内存序审计](docs/memory-model.md)
- [上游审计](docs/upstream-analysis.md)
- [准确的上游/衍生边界](UPSTREAM_DIFF.md)

benchmark report 固定到明确 revision；结果不是从上游复制，也不能推广到所记录 WSL2 host 与 synchronous ping-pong protocol 之外。

## 上游与归属

Primary upstream：

- [kyr0/libsharedmemory](https://github.com/kyr0/libsharedmemory)
- 固定提交：`9e24caaefb28826e99a33be2dd1350725558dd80`
- 许可证：MIT
- 上游版权原文保留于 [LICENSE](LICENSE)
- original commit 连历史导入，没有 squash 或修改日期

仍归属上游的部分：

- 历史 repository tree 与 development lineage；
- original MIT license 与 notice；
- 用作审计 baseline 的小型 CMake/CTest scaffold，以及 named-memory/fixed-capacity design concept。

FastIPC 重写或新实现的部分：

- compiled public API、layout、mapping lifecycle、queue algorithm、synchronization、liveness/recovery、transport adapter、status model、benchmark、fault test、sanitizer matrix、profiling 与设计/实现文档。

secondary repository 只作设计参考：

- `vt-tv/lockfree_ipc_ringbuffer`：比较 sequence/futex protocol；
- `rigtorp/ipc-bench`：比较 benchmark taxonomy。

compiled FastIPC core 没有 vendor 两者的源码。

导入的上游 example、FFI binding、旧 test、changelog、screenshot、single-header implementation 保留于 Git 历史；当前 tree 中仍存在的部分也只是 historical artifact，不进入当前 CMake build，其能力不算 FastIPC feature。未取得单独 destructive-file approval，因此没有大范围物理删除。

准确边界见 [UPSTREAM_DIFF.md](UPSTREAM_DIFF.md)。

## 局限

- SPSC 零拷贝与 MPMC copy 是独立 layout；MPMC 不提供 Loan/Take，也不提供 fan-out。
- MPMC 只承诺正常执行期间的并发正确性；producer 在 reserve 后、publish 前退出会留下 FIFO hole，恢复仍为 **INCOMPLETE**。

```text
Known limitation:
MPMC guarantees concurrent correctness during normal execution,
but producer termination after reservation and before publication
does not yet provide full robust recovery.
```

- 每个 endpoint 同时最多一个未完成 loan/sample；slot 与最大 payload 在 setup 时固定。
- shared-memory peer 必须使用相同 Linux ABI 与兼容 FastIPC layout。
- heartbeat 只提供 bounded suspicion，不能证明 paused process 永久死亡。
- 存活但暂停的 producer 持有可写 span 时不可安全撤销；replacement 返回 WouldBlock，直到旧 handle 自行归还或 PID/start-tick 确认进程死亡。
- consumer sample 为只读，role token 被替换后可至少一次重投递；不声称 exactly-once。
- layout major 已升级到 2；旧 shared-memory object 会以 LayoutMismatch 拒绝。
- 没有 authentication、encryption、namespace broker、SELinux policy integration、NUMA placement 或 real-time scheduling guarantee。
- UDS sealed-memfd 是 descriptor-assisted shared memory，不是 pure socket-copy throughput。
- iceoryx baseline 当前为 INCOMPLETE；依赖和专用 adapter 都可复核前不得产生数值结果。
- revision `906d0f2` 的 30 分钟 seeded soak 已保存完整压缩 JSONL、summary 和退出码并通过；2 小时、overnight 与 24 小时仍为 **INCOMPLETE**，短 CTest 和 30 分钟结果都不能替代更长阶段。
- 部署决策前必须在 native Linux 与 target hardware 重测 WSL2 数据。
