# FastIPC 内存模型

## 范围

本文审计 libsharedmemory baseline 之后引入的 Linux SPSC shared-memory channel。实现位于 `src/shared_memory_transport.cpp`，byte layout 位于 `src/shared_memory_layout.hpp`，public seam 为 `Transport`。

contract 面向相同 ABI 构建的 Linux process。实现对 `MAP_SHARED` memory 内自然对齐的 32/64-bit integer 使用 GCC/Clang atomic builtin；compile-time check 要求两种宽度始终 lock-free。FastIPC 不声称 ISO C++ abstract machine 单独保证 process-shared atomic 在所有平台可移植。

## Slot ownership

head/tail 是单调递增 64-bit cursor。

- producer 独占 head，也是 free slot 的唯一 writer；
- consumer 独占 tail，也是 published slot 的唯一 reader；
- producer 写 `SlotHeader` 与 payload，再 publish head；
- consumer observe head、copy slot，再 publish tail；
- producer observe released tail 后，slot 才能 reuse。

head/tail 位于不同 cache line。endpoint metadata、queue geometry、epoch、counter 也做 cache-line isolation。不存在双方共同修改的 shared count。

## Atomic-order 审计

| 字段 / 操作 | Writer order | Reader order | 原因 |
| --- | --- | --- | --- |
| `header.init_state` | release | acquire | validation 前可见 static layout field |
| `header.generation` | release store | acquire load | replacement producer 在 endpoint ownership/message 前 publish session identity |
| `endpoint.pid` | release | acquire | PID publication 发生在 endpoint metadata initialization 后 |
| `endpoint.process_start_ticks` | PID release 前初始化 | PID acquire 后读取 | PID + Linux start tick 区分 original process 与 recycled PID |
| `endpoint.role_token` | PID release 前初始化 | operation entry/heartbeat update 时 acquire | 即使 channel generation 不变，新 claim 也能 fence old endpoint |
| `producer_cursor.head` | relaxed load，release store | acquire load | release 发布 slot length/sequence/payload；acquire 使其可见；只有 producer 写 head |
| `consumer_cursor.tail` | relaxed load，release store | acquire load | release 表示 slot read 完成；producer acquire 防 early reuse；只有 consumer 写 tail |
| `data_epoch` | release fetch-add | acquire load | wake 前排序 state transition，并提供稳定 futex expected value |
| `space_epoch` | release fetch-add | acquire load | producer wake 前排序 tail publication |
| heartbeat | release store | acquire load | health observer 在此前 progress 后看见 heartbeat |
| operation sequence/counter | relaxed fetch-add | relaxed load | diagnostic 不发布 payload/ownership |

没有使用 sequentially consistent operation。global total order 会增加 coherence constraint，却不会强化 SPSC proof。

## Hot path 为什么没有 CAS

选定 queue 是 SPSC。head/tail 各只有一个 writer，所以 CAS 只会解决 role invariant 下不可能存在的 contention。duplicate role prevention 属于 setup，并用 kernel file lock 串行化。若加入 MPSC，需要全新的 reservation protocol/proof，不能靠在当前 layout 四处添加 CAS。

failed compare-exchange 因不发布内容，可用 relaxed；成功 ownership CAS 需 acquire-release。这些规则记录给可选 MPSC 设计，但当前 hot path 有意不执行 CAS。

## Futex epoch protocol

wakeup 从不被当作 condition；queue predicate 始终是 head 与 tail 的关系。

consumer empty path：

```text
acquire-load head and compare with local relaxed tail
acquire-load data_epoch into expected
recheck head versus tail
futex WAIT_BITSET(data_epoch, expected, absolute monotonic deadline)
```

producer full path 对称使用 `space_epoch`。recheck 关闭 lost-wakeup window。peer 若在 epoch snapshot 与 syscall 之间改 state，futex value 已不同，kernel 返回 `EAGAIN`。spurious wake 与 `EINTR` 都回到 predicate loop。

release-publish head/tail 后，对方 release-increment 自己的 epoch 并调用 `FUTEX_WAKE`。epoch wrap 安全，因为 waiter 仍检查 queue predicate；epoch 是 sleep-generation hint，不是 queue state。

## Absolute deadline

`Deadline` 只保存一个 steady-clock target。每次 kernel wait 前，FastIPC 从同一原始 target 计算剩余时长，转换为 `CLOCK_MONOTONIC` absolute timespec，再用 `FUTEX_WAIT_BITSET`。`EINTR`、`EAGAIN`、spurious wake 都不能延长 deadline。

## Peer death、active spin 与 bounded probe

queue full/empty 时先执行最多 `active_spin_count` 次 predicate check；peer 很快 publish/consume 时可避开 syscall，configured bound 防止 uncontrolled busy wait。

spin 后的 blocked operation 有两个 absolute deadline：caller deadline 与 next liveness probe，取更早者等待。liveness-probe timeout 只回到 queue predicate，不作为 caller timeout。每轮 blocked loop 读取 peer released PID/heartbeat。较昂贵的 `/proc/<pid>/stat` start-tick check 会 amortize：只在 heartbeat 已 expired 或 identity-probe timestamp 到期时执行，当前最多每 heartbeat interval 一次。missing PID 为 `PeerUnavailable`；消失/recycled identity 或 expired heartbeat 为 `PeerDead`。

每个 claimed endpoint 拥有一条 control-plane `jthread`，以 `clamp(peer_timeout / 4, 1 ms, 250 ms)` 更新 heartbeat，data plane idle 时也继续。thread 先校验 PID/start tick/role token，另一 endpoint claim role 后就退出。`Close` request stop、wake local condition variable、join heartbeat thread，最后才 clear metadata/unmap。

SIGKILL 不能 increment epoch，因此 bounded probe 避免 crash 后无限 futex sleep。graceful `Close` 以 release clear PID、increment peer epoch、立即 wake。dead/expired role takeover 位于 SPSC hot path 外，并由 `flock` 串行化。producer takeover 增加 channel generation；所有 role takeover 发布 fresh role token。`Send`/`Receive` 在触碰 ring 前校验 token，所以 resumed stale endpoint 得 `StaleGeneration`。这是 API boundary 的 cooperative fencing；若 process 在最后 ownership check 后、cursor publication 前冻结，严格 recovery 还需 generation-tagged slot/cursor。

## Buffer 与 corruption semantics

destination 太小或 slot length 超 configured max 时，consumer 不推进 tail。这样保留 evidence，避免 silent loss。recovery operation 可显式 abandon corrupt generation；普通 `Receive` 不做猜测。
