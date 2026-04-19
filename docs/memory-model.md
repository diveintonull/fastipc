# FastIPC 内存模型

## 范围

本文审计 libsharedmemory baseline 之后引入的 Linux SPSC shared-memory channel。实现位于 `src/shared_memory_transport.cpp`，byte layout 位于 `src/shared_memory_layout.hpp`，public seam 为 `Transport`。

contract 面向相同 ABI 构建的 Linux process。实现对 `MAP_SHARED` memory 内自然对齐的 32/64-bit integer 使用 GCC/Clang atomic builtin；compile-time check 要求两种宽度始终 lock-free。FastIPC 不声称 ISO C++ abstract machine 单独保证 process-shared atomic 在所有平台可移植。

## Slot ownership

head/tail 是单调递增 64-bit cursor，决定队列逻辑边界；每个固定 slot 另有带 generation/owner tag 的生命周期状态。

- producer `Loan` 以 CAS 把 `Free` 改为 `ProducerClaiming`，写入 owner tag 后 release-store `ClaimedByProducer`，再把可写 span 交给调用方；
- `Publish` 校验 cursor、chunk generation、owner generation、role token 与 channel generation，再以 CAS 发布 `Published`，最后以精确 expected-value CAS 推进 head；
- consumer `Take` 以 CAS 把 `Published` 改为 `ConsumerTaking`，写入 consumer owner tag 后 release-store `LoanedToConsumer`，只暴露只读 span；
- `Release` 校验所有 tag，以 CAS 直接发布 `Free`，不再写 slot，最后以精确 expected-value CAS 推进 tail；
- producer 只有 acquire-observe released tail 后才可复用 slot；
- copy `Send`/`Receive` 只是上述 loan API 的兼容包装：各有一次 payload copy，不维护第二套队列协议。

head/tail 位于不同 cache line。endpoint metadata、queue geometry、epoch、counter 也做 cache-line isolation。双方不共同写 head/tail，但 recovery endpoint 可能与恢复执行中的旧 endpoint 竞争同一 slot state，因此 slot ownership 必须原子化。

完整状态图和 crash 恢复矩阵见 [chunk-lifecycle.md](chunk-lifecycle.md)。

## Atomic-order 审计

| 字段 / 操作 | Writer order | Reader order | 原因 |
| --- | --- | --- | --- |
| `header.init_state` | release | acquire | validation 前可见 static layout field |
| `header.generation` | release store | acquire load | replacement producer 在 endpoint ownership/message 前 publish session identity |
| `endpoint.pid` | release | acquire | PID publication 发生在 endpoint metadata initialization 后 |
| `endpoint.process_start_ticks` | PID release 前初始化 | PID acquire 后读取 | PID + Linux start tick 区分 original process 与 recycled PID |
| `endpoint.role_token` | PID release 前初始化 | operation entry/heartbeat update 时 acquire | 即使 channel generation 不变，新 claim 也能 fence old endpoint |
| `slot.state` | 成功 CAS 为 acq_rel，稳定状态为 release store | acquire load；CAS 失败序为 acquire | 发布或观察 owner tag 与 payload；同时关闭 stale endpoint/recovery 的竞争窗口 |
| slot owner/generation tag | transient state 期间普通写 | stable state acquire 后普通读 | `ClaimedByProducer`/`LoanedToConsumer` 的 release publication 使整组 tag 一致可见 |
| slot payload | producer mutable loan 内普通写 | observe `Published` 后普通读 | `ClaimedByProducer -> Published` 的 acq_rel CAS 建立 payload happens-before；consumer span 只读 |
| `producer_cursor.head` | relaxed load，acq_rel CAS | acquire load；CAS 失败序为 acquire | 发布队列可见边界；旧 producer 与 replacement 只能有一个把精确 cursor 推进一格，不能回滚 head |
| `consumer_cursor.tail` | relaxed load，acq_rel CAS | acquire load；CAS 失败序为 acquire | 发布可复用边界；旧 consumer 与 replacement 只能有一个推进 tail |
| `data_epoch` | release fetch-add | acquire load | wake 前排序 state transition，并提供稳定 futex expected value |
| `space_epoch` | release fetch-add | acquire load | producer wake 前排序 tail publication |
| heartbeat | release store | acquire load | health observer 在此前 progress 后看见 heartbeat |
| operation sequence/counter | relaxed fetch-add | relaxed load | diagnostic 不发布 payload/ownership |

没有使用 sequentially consistent operation。global total order 会增加 coherence constraint，却不会强化 SPSC proof。

## 槽位与游标的两层 CAS

稳定会话内的队列仍是 SPSC；但崩溃接管会让旧端点与替代端点在极窄窗口共同尝试发布同一游标。普通存储可能让暂停后恢复的旧执行流把已经前进的 head/tail 写回旧值，因此游标发布使用 `expected == handle.cursor` 的 compare-exchange。只有胜者计数和唤醒；失败方返回或收敛到新游标。

槽位 CAS 负责借用生命周期与可复用/重投递稳定态；游标 CAS 只负责同一 SPSC 逻辑位置的幂等发布。两者都不是多生产者预留机制。若加入 MPMC，仍需全新的预留/游标协议与证明，不能把这些 CAS 直接宣称为 MPMC-safe。

## Futex 纪元协议

唤醒从不被当作条件；队列谓词始终是 head 与 tail 的关系。

消费者遇到空队列时：

```text
以 acquire 语义读取 head，并与本地 relaxed tail 比较
以 acquire 语义读取 data_epoch，保存为 expected
再次检查 head 与 tail
futex WAIT_BITSET(data_epoch, expected, 单调时钟绝对截止时间)
```

生产者遇到满队列时对称使用 `space_epoch`。二次检查关闭丢失唤醒窗口。对端若在纪元快照与系统调用之间改变状态，futex 值已经不同，内核返回 `EAGAIN`。伪唤醒与 `EINTR` 都回到谓词循环。

以 release 语义发布 head/tail 后，对方以 release 语义递增自己的纪元并调用 `FUTEX_WAKE`。纪元回绕是安全的，因为等待者仍会检查队列谓词；纪元只是睡眠代数提示，不是队列状态。

## 绝对截止时间

`Deadline` 只保存一个稳态时钟目标。每次进入内核等待前，FastIPC 都从同一原始目标计算剩余时长，转换为 `CLOCK_MONOTONIC` 绝对时间，再用 `FUTEX_WAIT_BITSET`。`EINTR`、`EAGAIN`、伪唤醒都不能延长截止时间。

## 对端死亡、有界主动自旋与探测

队列满/空时先执行最多 `active_spin_count` 次谓词检查；对端很快发布/消费时可以避开系统调用，配置上限则防止不受控忙等。

spin 后的 blocked operation 有两个 absolute deadline：caller deadline 与 next liveness probe，取更早者等待。liveness-probe timeout 只回到 queue predicate，不作为 caller timeout。每轮 blocked loop 读取 peer released PID/heartbeat。较昂贵的 `/proc/<pid>/stat` start-tick check 会 amortize：只在 heartbeat 已 expired 或 identity-probe timestamp 到期时执行，当前最多每 heartbeat interval 一次。missing PID 为 `PeerUnavailable`；消失/recycled identity 或 expired heartbeat 为 `PeerDead`。

每个 claimed endpoint 拥有一条 control-plane `jthread`，以 `clamp(peer_timeout / 4, 1 ms, 250 ms)` 更新 heartbeat，data plane idle 时也继续。thread 先校验 PID/start tick/role token，另一 endpoint claim role 后就退出。`Close` request stop、wake local condition variable、join heartbeat thread，最后才 clear metadata/unmap。

SIGKILL 不能 increment epoch，因此 bounded probe 避免 crash 后无限 futex sleep。graceful `Close` 以 release clear PID、increment peer epoch、立即 wake。role takeover 位于 SPSC cursor hot path 外，并由 `flock` 串行化；producer takeover 增加 channel generation，所有 takeover 发布 fresh role token。每个 loan 还记录 chunk generation、owner generation、PID/start tick、role token 与 channel generation，恢复和旧 handle 都必须精确匹配这些 tag。

producer loan 暴露可写 span，存活 process 中的旧写者无法被撤销。即使 heartbeat 过期或 role token 已替换，只要原 PID/start tick 仍存活，replacement `Loan` 就返回 `WouldBlock`，安全优先于可用性；确认进程死亡后才把 `ProducerClaiming`/`ClaimedByProducer` 直接 CAS 到 `Free`。若旧 producer 已把 slot 改为 `Published`、但尚未来得及推进 head，replacement 会用 cursor CAS 完成 publication；暂停后恢复的旧执行流不能回滚 head。

consumer sample 只读，因此稳定的 `LoanedToConsumer` 在 consumer role token 被替换或 owner 死亡后可恢复为 `Published` 并至少一次重投递；旧 sample 的 `Release` 因 tag 不匹配返回 `StaleGeneration`。`ConsumerTaking` 是 owner tag 尚可能写到一半的 transient state，原 process 仍存活时 replacement 返回 `WouldBlock`，只有确认死亡后才重投递。

## Buffer 与 corruption semantics

copy `Receive` 先 `Take`。若 destination 太小，它把 `LoanedToConsumer` 直接 CAS 回 `Published`，不推进 tail，再返回 `BufferTooSmall`；后续更大 buffer 或 zero-copy `Take` 仍能取得同一消息。

slot length 超 configured max 或 sequence 不匹配时返回 `CorruptData`，不推进 tail，保留故障 evidence，避免 silent loss。普通 `Receive` 不猜测如何丢弃 corrupt generation。
