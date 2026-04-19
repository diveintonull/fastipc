# FastIPC 上游分析

## 范围与证据

FastIPC 从 kyr0/libsharedmemory 的提交 `9e24caaefb28826e99a33be2dd1350725558dd80` 开始。上游是 C++20 header-only shared-memory library，含 POSIX、Windows、macOS path。本文审计 FastIPC redesign 前导入的源码；准确 provenance/tree hash 见工作区 [UPSTREAMS.md](../../../UPSTREAMS.md)。

固定上游的干净 Release/Ninja baseline 通过唯一 CTest target。这只证明 baseline 能构建并通过自己的 suite，不是新 FastIPC requirement 的证据。

## 原始架构

一个 public header 同时包含 platform mapping、stream-like last-value storage 与 queue。`lsm::Memory` 拥有 named mapping；higher-level stream 在这些 byte 上叠加 flag、revision、acknowledgement、payload length、writer lock、data；`lsm::SharedMemoryQueue` 在另一 mapping 上叠加 queue metadata 与 fixed-size slot。

没有 transport/protocol interface。caller 用 shared-memory name、size、persistence flag、role 构造 concrete stream/queue type。

## Shared-memory layout

上游 queue 的 host-native layout：

```text
offset  size  field
0       4     writeIndex
4       4     readIndex
8       4     capacity
12      4     atomic count
16      4     maxMessageSize
20      4     atomic producerLock
24      4     atomic consumerLock
28      ...   capacity fixed-size slots

slot := uint32 length + maxMessageSize bytes
```

writer 根据 constructor argument 计算 mapping size、unlink existing POSIX object、创建新 object、初始化 field。reader 按自己的 argument 计算 size 做 mapping，再用 mapping 内 value 覆盖本地 capacity/max-message。

layout 没有 magic、byte order、ABI-width marker、total-size、version、initialization state、generation、endpoint identity、heartbeat、checksum。reader 不校验 capacity、max message size、slot length，也不确认 object 足够容纳 trusted metadata，因此 old/malformed segment 可能被解释为 live queue。

## Queue structure 与数据流

Enqueue 获取 producer spin lock、检查 count、把 length/payload copy 到 current slot、推进 writeIndex、release-increment count、释放 lock。Dequeue 对称地获取 consumer lock、检查 count、copy 到 `std::string`、推进 readIndex、release-decrement count、释放 lock。Peek 获取 consumer lock，但不推进。

多个 producer/consumer 可在各自 side 串行执行，但 lock 不记录 owner identity。process 持 lock 退出会让未来 peer 永久 spin。

## Synchronization 与 C++ object model

queue 在 mapped byte 内 placement-construct `std::atomic<uint32_t>`，其他 process 用 `reinterpret_cast` 恢复 reference。side lock 使用 acquire CAS、relaxed failure、release store；count 使用 acquire load 与 release fetch-add/subtract；index/slot 为 ordinary byte，间接由 count 和 side lock ordering。

设计意图可理解，但上游没有记录 cross-process C++ object lifetime、审计 lock-free requirement 或证明每条 happens-before edge。FastIPC 改用 SPSC head/tail publication、cache-line isolation，并在 `docs/memory-model.md` 记录每种 ordering。

## 所有权与生命周期

`Memory` 拥有 fd 与 mapping。POSIX creator 无条件 `shm_unlink`，mode 0777 create，在检查 `shm_open` 前调用 `fchmod`，再 truncate/map。destruction 按 persistence flag unmap/close，可选 unlink。

没有 atomic creator election 或 initialization handshake。两个 creator 可 unlink/replace 同一 name，而 existing peer 仍指向 old object。name ownership、mapping ownership、endpoint role、generation 未分离，无法可靠识别 restart。

## Blocking model 与 backpressure

queue operation 不阻塞：full 时 enqueue false，empty 时 dequeue false；caller 自己 wait。side lock 用 yield busy-spin。没有 futex、epoch、absolute deadline、timeout status、cancellation 或 lost-wakeup protocol。

memory bounded 是有价值的 invariant，但 overflow 只有隐式 immediate failure。没有 Block/Timeout/Drop policy object，也没有解释 nondelivery 的 counter。

## 故障处理

mapping call 返回小型 Error enum，higher layer 多数 throw `runtime_error`。上游不检测/修复：

- incompatible/truncated layout；
- corrupt slot length；
- duplicate producer/consumer ownership；
- process 持 lock 死亡；
- stale persistent segment；
- peer restart/PID reuse；
- stalled-but-alive peer；
- 另一 operation wait 时 close。

## API 与性能特征

API 紧凑，却耦合 mapping、role、queue policy、payload、lifecycle。data copy 进出 fixed-size slot。双方共同更新 shared count，全部 28 B metadata 还共用一个 cache line，产生可避免 coherence traffic。yield lock 在 contention 下消耗 CPU/scheduler resource。上游没有 latency percentile、CPU、context switch、resident memory 记录，因此本审计不作性能声明。

## 修改边界

### 保留

- MIT license、notice、真实上游历史、attribution。
- 小型 CMake/CTest scaffold 与 Linux named-mapping RAII concept。
- fixed-capacity storage 作为 bounded-memory invariant。
- migration 期间只把 baseline test 当 compatibility evidence。

### 重写

- POSIX create/open、permission、size check、initialization election、mapping ownership、cleanup。
- byte layout：改为显式 versioned/validated Linux protocol。
- queue：改为 cache-line-isolated SPSC head/tail publication，并审计 acquire/release。
- error：timeout、peer death、layout mismatch、role conflict、corruption、shutdown 使用 status/result。
- backpressure：显式 Block/Timeout/Drop policy + metric。

### 不进入 compiled derivative

- focused Linux core 之外的 Windows/macOS implementation。
- 绕过新 lifecycle 的 legacy last-value stream 与 FFI。
- process-shared spin lock 与 shared count hot spot。
- 隐式 0777 permission 与 unlink-before-create。

当前 top-level CMake 只暴露新 `FastIPC::fastipc` library、test、benchmark。导入的 example、FFI、single-header implementation、old test 保留为历史文件，任何 current target 都不 include/link。未取得单独 destructive authorization，因此没有物理删除；这种 quarantine 既保留 attribution，也不把 legacy behavior 声称为 FastIPC capability。

### 新增

- magic、version、header/segment size、feature flag、generation、endpoint metadata、PID/start identity、heartbeat、initialization state、epoch；
- 使用 monotonic absolute deadline 的 futex wait，以及 recheck-before-sleep 关闭 lost-wakeup window；
- duplicate-role rejection、stale detection、owner-death reporting、restart、generation-aware reconnection；
- 带 shared-memory 与 Unix Domain Socket adapter 的 Transport interface；pipe 只作 benchmark baseline；
- unit、multiprocess、malformed-layout、fault-injection、sanitizer test；
- 64 B–1 MiB 可复现 benchmark，记录 throughput、P50/P95/P99、CPU、context switch、memory，再进行 measured profiling experiment。
