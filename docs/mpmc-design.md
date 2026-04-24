# FastIPC 有界 MPMC 设计

## 1. 范围与非目标

本阶段新增一个可被多个线程、多个进程并发调用的有界共享内存队列。它实现正常运行期间的 MPMC 并发正确性与争用基准，不改变现有 `SharedMemoryTransport` 的 SPSC、Loan/Take 或崩溃回收路径。

选择独立 `MpmcSharedMemoryTransport`，而不是给 SPSC 布局增加运行时模式分支，原因是两者的不变量不同：SPSC 的单写者 cursor 不需要 CAS，且零拷贝 handle 有 generation/owner 生命周期；MPMC 则必须先 CAS reserve 全局位置，再用每槽 sequence 发布或释放。拆开后，SPSC 热路径、共享内存 ABI 和已有故障恢复证明都不受影响。

本阶段 MPMC 使用 copy-in/copy-out，不提供 Loan API。零拷贝 MPMC 会把“reserve 后进程退出”和跨进程引用生命周期耦合在一起；在没有可证明的恢复协议前，不把它伪装成已完成能力。

## 2. 公共接口

```cpp
auto owner = MpmcSharedMemoryTransport::Create(config);
auto peer = MpmcSharedMemoryTransport::Open(config);

owner->Send(payload, options);
peer->Receive(destination, deadline);
```

`Create` 只负责独占创建和初始化共享对象；`Open` 只映射并校验已有对象。创建者与打开者都可以并发 `Send`/`Receive`，因此不存在单一 producer/consumer role 所有权。容量必须是至少 2 的 2 次幂，内存固定分配，运行期不扩容；为避免错误配置造成超大映射，单个 segment 上限为 512 MiB。

## 3. 共享内存布局

```text
MpmcSharedLayout
├── SegmentHeader: magic/version/size/init_state
├── QueueConfig: capacity/max_message_size/stride/offset
├── EnqueueCursor: enqueue_position + data_epoch + counters
├── DequeueCursor: dequeue_position + space_epoch + counters
└── Slot[capacity]
    ├── sequence
    ├── length
    └── payload[max_message_size]
```

cursor、futex epoch 和每个 slot header 分别按 cache line 对齐。槽 `i` 初始化为 `sequence=i`。

## 4. 算法与线性化点

Producer：

1. 读取 `enqueue_position`，定位 `position % capacity`。
2. acquire 读取 slot sequence。`sequence == position` 表示可用；较旧表示队列满；较新表示另一个 producer 已推动 cursor，需要重试。
3. CAS `enqueue_position: position -> position + 1`。成功的 CAS 是 enqueue reservation 的线性化顺序。
4. 写 `length` 和 payload。
5. release 写 `sequence = position + 1`，消息从此对 consumer 可见；递增 `data_epoch` 并 futex wake。

Consumer：

1. 读取 `dequeue_position`，期望 slot `sequence == position + 1`。
2. CAS `dequeue_position: position -> position + 1`，成功后独占该消息。
3. acquire sequence 保证能看到 producer 在 release publish 前的 payload 写入。
4. 读取/校验/copy payload。
5. release 写 `sequence = position + capacity`，允许下一轮 producer 复用；递增 `space_epoch` 并 futex wake。

队列 FIFO 由 reservation position 决定。后一个 producer 可以先完成 payload，但 consumer 不跳过尚未 publish 的较早 position；因此保留 FIFO，同时会产生队首阻塞。

## 5. 等待、背压与失败返回

热路径先进行有限 active spin。确认 full/empty 后，线程读取对应 epoch，再次检查条件，最后用基于 `CLOCK_MONOTONIC` 的 futex absolute deadline 睡眠；“读取 epoch → 重检 → wait(expected epoch)”避免丢失唤醒。

- full + `Drop`：返回 `Dropped`。
- full + 到达 deadline：返回 `Timeout`。
- empty + 到达 deadline：返回 `Timeout`。
- destination 太小：consumer 已经 reserve，无法安全回滚全局 dequeue cursor；实现会释放该 slot、把消息计为 dropped，并返回 `BufferTooSmall`，保证队列继续前进。

## 6. 正确性验证

- 单元：参数、FIFO、full/drop/timeout、empty timeout、buffer-too-small 后继续前进。
- 线程并发：4 producers × 4 consumers，所有 token 恰好一次，无重复、无丢失、checksum 正确。
- 跨进程集成：3 producers × 3 consumers 使用独立 mapping，父进程校验共享计数。
- futex：空队列 consumer 与满队列 producer 都要被对端操作唤醒。
- 故障刻画：子进程只完成 enqueue CAS reservation 就退出，验证后续已 publish 消息仍被未发布的队首 position 阻塞。
- sanitizer：Debug/Release/ASan/UBSan/TSan 矩阵。
- benchmark：1P1C、2P2C、4P4C 的可复核 contention 结果。

## 7. 已知限制与恢复边界

`INCOMPLETE`：当前没有 abandoned reservation 恢复协议。生产者在 CAS reserve 后、release publish 前退出时，consumer 不能跳过该 position，否则会破坏 FIFO；也不能仅凭超时 reclaim，因为 producer 可能只是被调度延迟。consumer 在 reserve 后退出同样可能在队列绕回时阻塞 producer。

README 必须保留以下边界声明：

```text
Known limitation:
MPMC guarantees concurrent correctness during normal execution,
but producer termination after reservation and before publication
does not yet provide full robust recovery.
```

在声称 crash-safe 之前，后续设计至少需要 reservation owner identity、可判定的进程 generation/liveness、不会与迟到写入竞争的 fencing，以及 FIFO/reclaim 的形式化不变量和故障注入证据。

## 8. 其他边界

- 依赖 Linux 在共享映射上的 lock-free 32/64-bit 原子操作与 futex。
- cursor 的模数比较假设活动位置距离小于 `2^63`；持续运行超过该边界尚未验证。
- 不提供多订阅 fan-out；每条消息只由一个 consumer 获取。
- `Close` 不回收其他 participant 的未完成 reservation，也不宣称 crash safety。

## 9. 验证快照

实现 revision：`6d9c7549666b6c064cce75dd6c4f19e34044dbfd`。

五套全量 CTest：

| 配置 | 结果 | 日志 |
| --- | ---: | --- |
| Debug | 27/27 | [日志](../tests/results/2026-08-21-debug-6d9c754.log) |
| Release | 27/27 | [日志](../tests/results/2026-08-21-release-6d9c754.log) |
| ASan | 27/27 | [日志](../tests/results/2026-08-21-asan-6d9c754.log) |
| UBSan | 27/27 | [日志](../tests/results/2026-08-21-ubsan-6d9c754.log) |
| TSan | 27/27 | [日志](../tests/results/2026-08-21-tsan-6d9c754.log) |

合计 135/135；sanitizer 日志无报告。MPMC 定向路径还在当前源码上独立通过 Debug 5/5、ASan 5/5、UBSan 5/5、TSan 5/5。

正式 Release contention 原始文件为 [JSONL](../benchmarks/results/2026-08-21-mpmc-contention-wsl2-gcc13-6d9c754.jsonl)，SHA-256 `5307be6000d7850cf2a52a4587dd2451c585a2bc3bed3d34dc5b454e8268333b`。1P1C/2P2C/4P4C × 64 B/1 KiB/64 KiB × 3 trial 共 27 条 result，全部 exact-count、零 missing/duplicate/checksum error/queue timeout。

这些证据支持正常执行期间的并发正确性与已测争用行为。它们不改变第 7 节的 crash 边界：abandoned producer reservation recovery 仍为 `INCOMPLETE`。
