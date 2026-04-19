# FastIPC 零拷贝设计

## 1. 目标与边界

零拷贝模块把同机 SPSC channel 从“应用缓冲区与共享 slot 之间复制”扩展为显式所有权传递：

- producer 通过 `Loan(size, options)` 借到可写 span；
- producer 写完后通过 `Publish()` 发布；
- consumer 通过 `Take(deadline)` 取得只读 span；
- consumer 处理完后通过 `Release()` 归还；
- 未显式提交或释放时，RAII 析构分别执行 `Abandon()` 或 `Release()`。

“零拷贝”只描述 FastIPC public seam 不再强制把 payload 复制进出临时应用缓冲区。应用是否扫描、改写或复制 payload，取决于 workload；benchmark 因此把 `transport_only` 与 `touch_memory` 分开。

本实现不改变以下边界：

- Linux、相同 ABI、POSIX shared memory；
- 单 producer、单 consumer；
- 每个方向一条单向 ring；
- 每个 endpoint 同时最多一个未完成 loan/sample；
- 不是 MPSC/MPMC，也不宣称 exactly-once；
- 不提供可撤销的远程可写映射。

## 2. Public API

核心接口位于 `include/fastipc/shared_memory_transport.hpp`：

```cpp
auto loan_result = producer->Loan(
    payload_size,
    {fastipc::BackpressurePolicy::Block,
     fastipc::Deadline::After(10ms)});
if (!loan_result) {
  return loan_result.status();
}
auto loan = std::move(loan_result).take_value();
Fill(loan.Data());
FASTIPC_RETURN_IF_ERROR(loan.Publish());

auto sample_result =
    consumer->Take(fastipc::Deadline::After(10ms));
if (!sample_result) {
  return sample_result.status();
}
auto sample = std::move(sample_result).take_value();
Consume(sample.Data());
return sample.Release();
```

`PublisherLoan` 与 `SubscriberSample` 都是 move-only RAII handle。成功操作后 handle 失效，`Data()` 返回空 span，重复 `Publish()` 返回 `Closed`，重复 `Release()` 是幂等成功。

现有复制 API 没有分叉出另一套协议：

- `Send` = `Loan` + `memcpy` + `Publish`；
- `Receive` = `Take` + `memcpy` + `Release`；
- destination 太小时，`Receive` 把 sample 原子地重投递，不推进 tail。

因此 copy 与 zero-copy 共用同一个 slot lifecycle、恢复协议和统计口径。

## 3. 深模块边界

零拷贝模块把复杂性封装在一个窄接口之后：

```text
应用
    |
    +-- 复制接口：Send / Receive
    |
    +-- 所有权接口：Loan / Publish / Take / Release
                         |
                         v
                 固定映射块池
                         |
                         v
             槽位状态 + 代数 + 所有者身份
```

调用方只处理类型化结果、span 和 RAII 句柄；布局版本、futex 纪元、PID/启动时钟刻度、角色令牌、游标发布与崩溃恢复都留在模块内部。这个接口也是后续 AutoRuntime 大消息适配器的唯一接入点。

## 4. 固定池与稳态分配

共享段在 channel 创建时一次性确定：

```text
segment =
    SharedLayout
  + slot_count * AlignUp(sizeof(SlotHeader) + max_message_size, 64)
```

每个 slot 的 payload 容量固定，`Loan(size)` 只接受 `size <= max_message_size`。队列满时使用 `Block`、`Timeout` 或 `Drop`；不会扩容，也不会创建 overflow list。

RAII handle 的状态内联保存：已有 endpoint `shared_ptr` 的控制块只做引用计数增减，不为每条消息创建 PImpl。`fastipc.zero_copy.no_allocation` 在完成 setup 与 warmup 后启用 test-only `operator new` 计数器，验证一次 `Loan/Publish/Take/Release` 成功路径的普通堆分配数为 0。

这项证据只覆盖成功的 warmed path；端点创建、错误字符串、benchmark copy buffer 与外部应用逻辑不在该断言内。

## 5. 所有权与线性化点

完整状态机见 [chunk lifecycle](chunk-lifecycle.md)。主要线性化点如下：

| 操作 | 线性化点 | 发布内容 |
| --- | --- | --- |
| `Loan` | `Free -> ProducerClaiming` CAS，随后 release-store `ClaimedByProducer` | size、sequence、chunk generation、producer identity |
| `Publish` | `ClaimedByProducer -> Published` CAS | payload 不再可写 |
| publish cursor | CAS `head: cursor -> cursor + 1` | consumer acquire-load 后可见完整 payload；旧 endpoint 不能回滚 cursor |
| `Take` | `Published -> ConsumerTaking` CAS，随后 release-store `LoanedToConsumer` | consumer owner identity |
| `Release` | `LoanedToConsumer -> Free` CAS | consumer 不再访问 payload；成功后不再写 slot |
| release cursor | CAS `tail: cursor -> cursor + 1` | producer acquire-load 后可复用 slot；只有 CAS 胜者计数和 wake |

临时状态 `ProducerClaiming` 与 `ConsumerTaking` 不是装饰：它们让恢复端区分“owner metadata 尚未写全”与“稳定借用已建立”。

没有通用 `Reclaimable` 中间态。`Abandon`/`Release`/`Requeue` 分别一次 CAS 到 `Free`/`Free`/`Published`，线性化后不再清 metadata 或改 slot state；下一次 claim 会覆盖失效 owner tag。这避免 replacement 已复用 slot 后被旧清理流程覆盖。

## 6. Generation fencing

每个 handle 捕获四组 tag：

- `chunk_generation`：slot 每次 producer claim 都递增；
- `owner_generation`：owner 每次变化都递增；
- `owner_role_token`：每次 endpoint claim 都重新生成；
- `owner_channel_generation`：producer replacement 推进的 channel generation。

`Publish`、`Abandon`、`Release` 与内部 `Requeue` 都要求 state 与全部 tag 精确匹配。旧 handle 即使在同一 PID 中恢复，也不能改变已经重新分配的 chunk。

PID 还与 Linux `/proc/<pid>/stat` start tick 配对，避免 PID recycle 被误判为原进程。

## 7. Crash 与暂停语义

### 7.1 Producer 在 Publish 前死亡

可写 loan 可能停在 `ProducerClaiming` 或 `ClaimedByProducer`。恢复端只有在 PID/start-tick 证明原进程已死亡后才把它直接 CAS 到 `Free`。

heartbeat 过期不足以回收可写 span。原因是暂停但仍存活的进程保留有效映射，恢复后仍能直接写 payload；用户态 generation 无法撤销这次内存写。

因此：

- replacement producer 可以取得 role；
- 若旧 producer 仍存活且持有未完成可写 loan，新 `Loan` 返回 `WouldBlock`；
- 旧进程恢复后，旧 `Publish` 因 role token/generation 失配返回 `StaleGeneration`，其 `Abandon` 安全归还尚未复用的 slot；
- 或旧进程真正死亡后，由 replacement 回收。

这是刻意选择的 safety-over-availability 边界，不把 heartbeat suspicion 伪装成内存撤销能力。

### 7.2 Producer 在 Published 后、head 前死亡

`Published` 表示 payload 已冻结。replacement 可以验证 sequence/length 后补做 head publication；消息按旧 session 的已提交内容交付。统计为 producer reclaim。

### 7.3 Consumer 持有 sample 时死亡

consumer span 是只读的。原进程死亡后，replacement 把 `LoanedToConsumer` 直接 CAS 回 `Published`，同一消息至少一次重投递。

### 7.4 Consumer 暂停后被接管

稳定的 `LoanedToConsumer` 可依据 role token 失配安全重投递，因为旧 handle 只能读；旧 `Release` 的 owner generation/token 检查会失败，不能释放 replacement 正在持有的 sample。

若暂停发生在 `ConsumerTaking` 临时状态且 owner metadata 尚未稳定，则仍等待原进程死亡或自行完成，避免旧执行流恢复后覆盖新 state。

### 7.5 Close 与 outstanding handle

`SharedMemoryTransport` 析构立即执行逻辑 `Close`：停止 heartbeat、清除自己仍拥有的 role、唤醒 waiter。mmap 的物理生命周期由 endpoint 与 outstanding handle 共享；最后一个引用销毁后才 `munmap`。因此 transport 对象先销毁不会造成 handle UAF。

## 8. Backpressure

`Loan` 沿用现有 queue predicate：

| Policy | queue 满时结果 |
| --- | --- |
| `Block` | bounded spin 后 futex wait，受 absolute deadline 与 peer probe 约束 |
| `Timeout` | 使用同一等待路径，到原始 monotonic deadline 返回 `Timeout` |
| `Drop` | 立即返回 `Dropped`，增加 drop counter |

“被活的旧 producer loan 占住”不是普通 queue-full；返回 `WouldBlock`，让调用方明确看到不可安全撤销的 owner。

## 9. 统计与可观测性

`TransportStats` 新增：

- `zero_copy_loans`；
- `zero_copy_publishes`；
- `zero_copy_takes`；
- `zero_copy_releases`；
- `producer_loan_reclaims`；
- `consumer_loan_reclaims`。

counter 使用 relaxed atomic，只做诊断，不参与 payload publication。

## 10. 验证证据

Debug CTest 当前 22/22，其中零拷贝专项覆盖：

- publish 前不可见、sample pin 住 slot；
- RAII abandon/release；
- copy 与 loan API 互操作；
- warmed success path 普通堆分配为 0；
- producer 在 Publish 前 SIGKILL 后回收；
- consumer 持 sample SIGKILL 后重投递；
- endpoint 关闭但 handle 仍持 mmap；
- producer SIGSTOP + lease takeover 时不复用可写 chunk；
- consumer SIGSTOP + takeover 时旧 Release 无法影响新 sample。

完整 Release benchmark 见 [零拷贝结果](../ZERO_COPY_BENCHMARK_RESULTS.md)。

## 11. 未完成项

- `INCOMPLETE`：MPSC/MPMC reservation、reservation hole recovery；
- `INCOMPLETE`：跨 NUMA placement/prefault/huge-page policy；
- `INCOMPLETE`：对恶意同 UID 进程的访问控制强化；
- `INCOMPLETE`：可撤销 writable mapping；当前明确采用进程死亡证明；
- `INCOMPLETE`：AutoRuntime 大消息 adapter，将在独立集成步骤接入。
