# FastIPC chunk 生命周期

## 1. 状态图

每个定长槽位的 64 字节头保存状态、长度、序号、块代数与所有者身份。

```text
                         生产者
Free
  | CAS
  v
ProducerClaiming
  | 写入长度 / 序号 / 所有者 / 各代数
  | 以 release 语义存储
  v
ClaimedByProducer
  | 生产者写入载荷
  | Publish CAS
  v
Published
  | compare-exchange head: cursor -> cursor + 1
  | 消费者以 acquire 语义读取 head
  | Take CAS
  v
ConsumerTaking
  | 写入消费者所有者身份
  | 以 release 语义存储
  v
LoanedToConsumer
  | 消费者只读载荷
  | Release CAS：直接发布 Free
  v
Free
  | compare-exchange tail: cursor -> cursor + 1
```

状态变为 `Free` 或 `Published` 后，原句柄不再写槽位；旧所有者标签可以留在头中，下一次稳定认领会覆盖它。这样稳定状态 CAS 本身就是线性化点，不存在“已发布可复用、随后旧清理者又改元数据”的 ABA 窗口。

## 2. 正常路径

### 2.1 Loan

1. producer relaxed-load 自己的 head，acquire-load consumer tail；
2. 检查 bounded ring 是否有空间；
3. 对目标 slot 做 `Free -> ProducerClaiming` CAS；
4. 写 length、sequence、chunk generation、owner generation、PID/start tick、role token、channel generation；
5. release-store `ClaimedByProducer`；
6. 返回 move-only `PublisherLoan`。

`ProducerClaiming` 防止恢复者把“已占位但 identity 尚未全部发布”误当成稳定 loan。

### 2.2 Publish

1. 校验 endpoint 仍拥有 producer role；
2. 校验 channel generation 与 handle 捕获值；
3. 校验 cursor、state 和四组 slot tag；
4. CAS `ClaimedByProducer -> Published`；
5. compare-exchange `head: cursor -> cursor + 1`；
6. 增加 publish/sent counter，唤醒 data futex。

`Published` CAS 后，handle 不再提供可写 span。若进程在 CAS 后、head 前死亡，replacement 可以完成 cursor publication；若旧执行流随后恢复，它的 cursor CAS 只会失败，不能把已经前进的 head 写回旧值。

### 2.3 Take

1. consumer relaxed-load tail，acquire-load producer head；
2. 校验 sequence 与 length；
3. CAS `Published -> ConsumerTaking`；
4. 更新 consumer owner generation、PID/start tick、role token 与 channel generation；
5. release-store `LoanedToConsumer`；
6. 返回只读 `SubscriberSample`。

### 2.4 Release

1. 校验 tail、state 与四组 tag；
2. CAS `LoanedToConsumer -> Free`；
3. compare-exchange `tail: cursor -> cursor + 1`；
4. cursor CAS 胜者增加 release/received counter，唤醒 space futex。

slot 只有在 consumer 明确放弃 span 后才回到 producer。`Free` 发布后，Release 不再读写 slot；即使 replacement 先推进 tail 并让 producer 复用，旧执行流也只能在 tail CAS 上失败。

## 3. RAII 路径

### 3.1 未发布 loan 析构

`PublisherLoan::~PublisherLoan` 调用 `Abandon`：

```text
ClaimedByProducer
  -> Free（单次 CAS）
```

head 不推进，因此下一次 producer loan 仍使用同一个 cursor。若 tag 已变化，旧 handle 返回 `StaleGeneration`，不触碰 slot。

### 3.2 未显式释放 sample 析构

`SubscriberSample::~SubscriberSample` 调用 `Release`，与显式释放相同。move-from handle 的 `active=false`，析构不重复操作。

### 3.3 Receive buffer 太小

复制 API 的 `Receive` 不能丢掉消息：

```text
LoanedToConsumer
  -> Published（单次 CAS）
```

tail 不推进，下一次 `Take/Receive` 重新看到同一 sequence。

## 4. 恢复矩阵

| 观察到的 state | 恢复条件 | 动作 | 交付语义 |
| --- | --- | --- | --- |
| `Free` | producer head 指向它 | 正常 claim | 无消息 |
| `ProducerClaiming` | 原 PID/start tick 存活 | `WouldBlock` | 不复用可能恢复的可写执行流 |
| `ProducerClaiming` | 原进程确认死亡 | CAS 到 `Free` | 未提交消息丢弃 |
| `ClaimedByProducer` | 原进程存活 | `WouldBlock` | 不撤销 mutable span |
| `ClaimedByProducer` | 原进程死亡 | CAS 到 `Free` | 未提交消息丢弃 |
| `Published` 且 head 未推进 | metadata 合法 | CAS `head: cursor -> cursor + 1` | 已提交消息交付 |
| `ConsumerTaking` | 原进程存活 | `WouldBlock` | 等待临时写入完成 |
| `ConsumerTaking` | 原进程死亡 | CAS 回 `Published` | 至少一次重投递 |
| `LoanedToConsumer` | 当前 owner endpoint 仍有效 | `WouldBlock` | sample 继续 pin |
| `LoanedToConsumer` | owner 死亡或 role token 已被替换 | CAS 回 `Published` | 至少一次重投递 |
| `Free` 且 tail 未推进 | cursor 恢复 | CAS `tail: cursor -> cursor + 1` | release 完成 |

cursor CAS 只接受调用方捕获的精确旧值。旧执行流在 replacement 已推进多个消息后恢复时只能失败，不能用普通 store 把单调 cursor 回滚。

## 5. 为什么 producer 与 consumer 的暂停语义不同

producer 拿到 `span<byte>`，可直接写共享页。generation 只能保护协议操作，不能阻止一条已经获得的机器指令写内存。因此活着的旧 producer 不可被安全回收。

consumer 拿到 `span<const byte>`。稳定 sample 被接管后，旧进程最多继续读取；它的 `Release` 必须通过 owner generation/token CAS，因此不能释放 replacement 的 sample。这个不对称是 API constness 带来的真实安全边界。

## 6. Tag 与 ABA

单独检查 state 不足以防 ABA。例如旧 handle 看到的 `ClaimedByProducer` 可能已经经历：

```text
Claimed(old) -> Free -> Claimed(new)
```

因此操作同时匹配：

```text
state
chunk_generation
owner_generation
owner_role_token
owner_channel_generation
cursor
```

任一不匹配都返回 `StaleGeneration`。generation 使用非零递增计数；64-bit wrap 被视为工程上不可达，但实现仍跳过 0。

稳定态 CAS 成功后，handle 不再清 metadata 或重写 slot state；这样新 owner 看见稳定态后复用 slot 时，不会被旧清理流程覆盖。head/tail 另用精确 expected-value CAS 仲裁旧 endpoint 与 replacement 的 cursor publication。

## 7. Memory ordering

- owner metadata 写完后 release-store 稳定 state；
- 观察稳定 state 使用 acquire-load；
- slot state 与 head/tail CAS 成功序使用 acquire-release，失败序使用 acquire；
- payload 先由 `Published` CAS 发布，再由 head CAS 发布队列边界；
- slot 先由 `Free` CAS 发布不可访问，再由 tail CAS 发布可复用边界；
- epoch 只负责 sleep/wakeup，不代表 queue truth。

详细审计见 [memory model](memory-model.md)。

## 8. 不声称的保证

- 不声称 exactly-once；consumer crash 后允许同一 sequence 重投递；
- 不声称 heartbeat 能证明进程死亡；
- 不声称 paused mutable owner 可被回收；
- 不声称多 producer reservation 安全；
- 不声称对恶意进程写共享页有隔离。
