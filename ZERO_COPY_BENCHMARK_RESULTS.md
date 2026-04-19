# FastIPC 零拷贝 benchmark 结果

## 1. 证据

本报告来自两次真实 Release run：

- [FastIPC Copy 原始 JSONL](benchmarks/results/2026-08-20-zero-copy-copy-wsl2-gcc13.jsonl)
- [FastIPC Zero-copy 原始 JSONL](benchmarks/results/2026-08-20-zero-copy-loan-wsl2-gcc13.jsonl)

每个文件包含 1 条 environment record 与 12 条 result：六种 payload × 两种访问模式。原始行保存 Msg/s、MiB/s、P50/P95/P99/P99.9、user/system CPU、CPU utilization、voluntary/involuntary context switch、parent/child peak RSS。

## 2. 环境与身份

| 字段 | 值 |
| --- | --- |
| Host | `LAPTOP-NUKUM2JI`，WSL2 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel Core Ultra 9 275HX，24 online logical CPU |
| Compiler / build | GNU 13.3.0 / Release |
| Protocol | cross-process、single-outstanding ping-pong；latency 为 RTT |
| Clock / quantile | `steady_clock` / nearest-rank |
| Embedded revision | `b03da127e629` |

embedded revision 是 benchmark 前的 parent HEAD，因为本项目遵循“先 benchmark、后 commit”的实现顺序；它不是对 dirty worktree 的虚假精确标识。代码进入本地 commit 后会再做一次 exact-revision refresh。当前结果对应的文件 SHA-256 会在 commit 前最终核对。

## 3. 两种 workload

### `transport_only`

producer/consumer 只读写前 8 B sequence。copy API 仍必须搬运完整 payload；zero-copy 只测 ownership transfer、state transition 与 wakeup。这里报告的 MiB/s 是按逻辑 message size 推导的 transport capacity，不表示应用真实扫描了相同字节量。

### `touch_memory`

每个 sender 在目标存储中写完整 deterministic payload，每个 receiver 扫描并验证全部字节。zero-copy 直接在 loan/sample span 上执行；copy 路径还承担 API 强制的 slot 与应用 buffer 复制。

## 4. Transport-only

| Payload | Copy Msg/s | Zero-copy Msg/s | Z/C | Copy P50 us | Zero P50 us | Copy P99 us | Zero P99 us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 168,488 | 145,182 | 0.862x | 2.832 | 18.641 | 47.836 | 50.509 |
| 256 B | 131,497 | 139,772 | 1.063x | 19.816 | 19.993 | 51.743 | 49.759 |
| 1 KiB | 144,051 | 126,034 | 0.875x | 19.535 | 20.232 | 47.272 | 49.253 |
| 4 KiB | 109,408 | 136,959 | 1.252x | 20.896 | 19.747 | 53.013 | 48.428 |
| 64 KiB | 58,153 | 96,314 | 1.656x | 30.688 | 19.098 | 84.232 | 45.644 |
| 1 MiB | 11,335 | 88,331 | 7.793x | 163.591 | 19.158 | 350.479 | 85.725 |

1 MiB transport-only 的 88,331 logical Msg/s 或 86,261 MiB/s 不是“应用处理 86 GiB/s”的结论：payload 没被扫描。它准确说明 copy 路径随 payload 线性付费，而 loan/take ownership path 基本不随未触碰 payload 增长。

64 B 和 1 KiB 的单次结果没有显示优势，且 64 B P50 受双峰调度/active-spin 行为影响明显。小消息不能用“零拷贝一定更快”概括。

## 5. Touch-memory

| Payload | Copy Msg/s | Zero-copy Msg/s | Z/C | Copy P50 us | Zero P50 us | Copy P99 us | Zero P99 us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 149,915 | 129,350 | 0.863x | 19.204 | 20.058 | 46.987 | 57.376 |
| 256 B | 111,708 | 127,510 | 1.141x | 20.893 | 20.139 | 51.068 | 52.313 |
| 1 KiB | 85,906 | 88,602 | 1.031x | 22.962 | 22.693 | 56.967 | 56.101 |
| 4 KiB | 62,316 | 62,948 | 1.010x | 28.821 | 28.691 | 68.428 | 66.649 |
| 64 KiB | 11,981 | 12,844 | 1.072x | 159.341 | 145.616 | 240.103 | 255.857 |
| 1 MiB | 850 | 953 | 1.122x | 2,351.467 | 2,087.971 | 2,635.482 | 2,206.926 |

在完整触碰内存时：

- 1 MiB zero-copy Msg/s 高 12.2%，P50 RTT 低 11.2%，P99 低 16.3%；
- 64 KiB Msg/s 高 7.2%、P50 低 8.6%，但 P99/P99.9 单次 run 更差；
- 1–4 KiB 基本持平；
- 64 B zero-copy 更慢约 13.7%。

这支持“零拷贝价值随 payload 与应用访问方式变化”，不支持 universal winner。

## 6. CPU、context switch 与 RSS

所有原始字段均在 JSONL。解释规则：

- CPU 是 parent + child measured-window `getrusage` delta，可能超过 100%；
- context switch 是两个进程的 delta；
- RSS 是 process-lifetime peak，不是 per-message allocation；
- no-allocation 结论来自独立 CTest 的 warmed `operator new` probe，不由 RSS 推断。

## 7. 限制

- 每个 case 只有一次 run，未给置信区间；
- WSL2 scheduler、频率与 host load 会造成抖动；
- RTT 不是 one-way latency；
- synchronous ping-pong 不代表 saturated pipeline；
- `transport_only` 不触碰 payload；
- 当前没有 CPU pinning、NUMA binding 或 realtime scheduling；
- `INCOMPLETE`：iceoryx 同协议对照将在完整 comparative benchmark 阶段加入；若构建失败会保留失败日志；
- `INCOMPLETE`：native Linux 与目标机器人硬件复测。
