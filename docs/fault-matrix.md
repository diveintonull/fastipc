# FastIPC 故障矩阵

## 契约

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
| Version Mismatch | mapped major version 改为 unsupported | 精确 2.0 布局校验返回 `LayoutMismatch` | `fastipc.fault.version_mismatch` |
| Stale Shared Memory | SIGSTOP producer 超过 heartbeat lease，再 reclaim role | 新 generation 成功；resume old producer 得 `StaleGeneration` | `fastipc.fault.stale_shared_memory` |
| Queue Full | 填满 ring，调用 Drop 与 finite Timeout | `Dropped`、`Timeout`，两个 counter 各增一次 | `fastipc.fault.queue_full` |
| Queue Empty | block consumer，稍后 publish，经 `data_epoch` wake | receive exact payload，无 lost wakeup | `fastipc.fault.queue_empty` |
| Rapid Restart | consumer 保持 mapping，producer role 连续 reclaim 16 次 | generation 严格递增，每轮恢复消息流 | `fastipc.fault.rapid_restart` |
| Producer Loan Crash | child 在 `Loan` 后、`Publish` 前退出 | replacement 确认 PID/start tick 已死亡后回收 mutable loan，随后恢复发布 | `fastipc.fault.producer_loan_crash` |
| Consumer Sample Crash | child `Take` 后持有只读 sample 退出 | replacement 把 sample 重投递并读取同一 payload，旧 ownership 不推进 tail | `fastipc.fault.consumer_sample_crash` |
| Outstanding Handle Close | transport 在 loan/sample 存活时 `Close` 和析构 | RAII handle 保持 mapping；访问不悬空，结束时返回 typed status | `fastipc.fault.outstanding_handle_close` |
| Producer Paused Takeover | producer 持 mutable loan 后暂停；replacement claim role | 存活旧写者使新 `Loan` 返回 `WouldBlock`；旧 handle 恢复后只能 `StaleGeneration`/安全 abandon | `fastipc.fault.producer_paused_takeover` |
| Consumer Paused Takeover | consumer 持只读 sample 后暂停；replacement claim role | replacement 至少一次重投递；旧 `Release` 得 `StaleGeneration`，不能释放新 sample | `fastipc.fault.consumer_paused_takeover` |

聚合测试还覆盖 duplicate producer/consumer、idle-heartbeat false-takeover prevention、stale-consumer role-token fencing、graceful-close wakeup、message exchange 与 2,000-message epoch stress。

## Seeded Chaos 序列

`fastipc_chaos_runner` 不替代上面的单故障测试；它把公开 API 场景串成可复现的长序列，并在每个操作后先排空 outstanding message，再做一次 publish/take 恢复探针。

| 操作 | 注入点 | 操作内必须证明 |
| --- | --- | --- |
| `KillProducer` | child 已 `Loan` 但未 `Publish` 时 `SIGKILL` | replacement producer 回收 abandoned mutable loan；恢复探针通过 |
| `KillConsumer` | child 已 `Take` 且未 `Release` 时 `SIGKILL` | replacement consumer 重投递并校验同一 sequence/payload |
| `RestartProducer` | producer graceful stop 后重新打开同一 segment | generation/reclaim 后数据流恢复 |
| `RestartConsumer` | consumer graceful stop 后重新打开同一 segment | role 重领后数据流恢复 |
| `SlowConsumer` | ring 填满，consumer 延迟 release | blocked producer 经 space epoch 唤醒，所有 sequence 被顺序消费 |
| `QueuePressure` | ring 填满后分别使用 `Drop` 与 finite `Timeout` | 返回精确 typed status，两个统计 counter 都增加 |
| `Timeout` | live 但为空的 channel 上 finite `Take` | 返回 expected `Timeout`，receive-timeout counter 增加 |
| `DelayWakeup` | consumer 先阻塞，延迟 publish | data epoch 唤醒，无 lost wakeup，payload 校验通过 |

每个完整 8 操作周期都是八类操作的 seed-shuffled permutation。相同 seed 和 operation count 产生相同操作顺序；Linux 调度时序不因此变成 deterministic。逐行证据与长时边界见 [chaos-testing.md](chaos-testing.md)。

revision `906d0f2` 的 30 分钟 seed `20260821` 运行完成 168,954 次操作且正确性错误为 0；2 小时、overnight 和 24 小时仍为 `INCOMPLETE`。原始证据与 summary 见 [测试矩阵](../TEST_MATRIX.md#2026-08-21-seeded-chaos--soak-增量验证)。

当前 `fault` 标签共有 20 个 registered entry：17 个 SPSC/零拷贝直接故障、1 个 MPMC abandoned-reservation 限制测试、`fastipc.chaos.summary` 与 `fastipc.chaos.seeded_smoke`。Chaos 两项会串行覆盖八类操作，不能把它们再按“八个独立 CTest”相加。

## 复现

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L fault --output-on-failure
```

`fastipc.transport`、`fastipc.zero_copy` 与 `fastipc.mpmc` 是覆盖面更广的聚合测试；registered entry、内部 case 和一次 Chaos 序列是三个不同层级，报告时必须分开。
