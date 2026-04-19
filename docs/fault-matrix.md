# FastIPC 故障矩阵

## Contract

每种要求的故障都作为独立 CTest 暴露。只有 corruption 本身是测试主题时，才从 public adapter 外部注入；所有 observation 与 recovery assertion 都使用 SharedMemoryTransport public API。

| 场景 | 注入方式 | 预期可观察结果 | CTest |
| --- | --- | --- | --- |
| Peer Missing | POSIX SHM object 存在前 open consumer | `PeerUnavailable`，native detail 保留 ENOENT | `fastipc.fault.peer_missing` |
| Producer Crash | child 创建 producer，SIGKILL 后 consumer blocking | bounded liveness probe 内 `PeerDead`；replacement producer 推进 generation 并恢复流 | `fastipc.fault.producer_crash` |
| Consumer Crash | child 创建 consumer，SIGKILL 后填满 ring 再 send | `PeerDead`；replacement consumer claim role、drain committed message、恢复流 | `fastipc.fault.consumer_crash` |
| Restart | graceful close persistent producer，并 reclaim 同 segment | generation 增加，existing consumer adopt | `fastipc.fault.restart` |
| Slow Consumer | 填满 capacity 2 ring，延迟一次 receive | blocking producer sleep，在 `space_epoch` wake，无 unbounded allocation | `fastipc.fault.slow_consumer` |
| Timeout | live empty channel 上用固定 deadline Receive | `Timeout`，deadline 不延长 | `fastipc.fault.timeout` |
| Malformed Header | persistent producer close 后修改 mapped magic | public open 返回 `LayoutMismatch` | `fastipc.fault.malformed_header` |
| Version Mismatch | mapped major version 改为 unsupported | exact 1.1 validation 返回 `LayoutMismatch` | `fastipc.fault.version_mismatch` |
| Stale Shared Memory | SIGSTOP producer 超过 heartbeat lease，再 reclaim role | 新 generation 成功；resume old producer 得 `StaleGeneration` | `fastipc.fault.stale_shared_memory` |
| Queue Full | 填满 ring，调用 Drop 与 finite Timeout | `Dropped`、`Timeout`，两个 counter 各增一次 | `fastipc.fault.queue_full` |
| Queue Empty | block consumer，稍后 publish，经 `data_epoch` wake | receive exact payload，无 lost wakeup | `fastipc.fault.queue_empty` |
| Rapid Restart | consumer 保持 mapping，producer role 连续 reclaim 16 次 | generation 严格递增，每轮恢复消息流 | `fastipc.fault.rapid_restart` |

aggregate coverage 还包含 duplicate producer/consumer、idle-heartbeat false-takeover prevention、stale-consumer role-token fencing、graceful-close wakeup、message exchange 与 2,000-message epoch stress。

## 复现

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L fault --output-on-failure
```

fault label 恰好包含 specification 要求的 12 个 scenario。unlabelled `fastipc.transport` 在一个 process 中运行更广的 aggregate suite。
