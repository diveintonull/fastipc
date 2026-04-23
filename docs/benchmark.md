# FastIPC 统一 Benchmark 设计

## 1. 目标

本框架只比较行为可对齐的跨进程 ping-pong transport，并把环境、可用性、配置和结果写成可校验 JSONL。它不把线程内 queue microbenchmark 与 IPC 混为同一张性能表，也不因外部 baseline 缺失而生成虚构数字。

必测 transport：

- Linux pipe；
- Unix Domain Socket；
- FastIPC copy；
- FastIPC zero-copy；
- iceoryx：只有真实依赖和 adapter 可用时才测，否则输出机器可读 `baseline_status` 并标记 **INCOMPLETE**。

本机预检没有找到 `iceoryx_posh` 的 CMake package、pkg-config entry 或 shared library。因此当前 revision 不产生 iceoryx result；这不是 0 throughput，也不是“FastIPC 获胜”。

## 2. 公平性边界

所有可用 transport 使用同一协议：

1. parent 与 child 是两个真实进程；
2. single-outstanding request/echo；
3. 相同 payload、iteration、warmup 和 operation timeout；
4. parent 用 `std::chrono::steady_clock` 记录 RTT；
5. 一个 RTT 计 2 条 logical message、传输 `2 * payload_bytes`；
6. CPU 与 context switch 汇总 parent/child measured-window delta；
7. RSS 是 process-lifetime peak，只作为 coarse signal。

rigtorp SPSC 只能作为 queue algorithm microbenchmark；它不包含进程切换、内核 IPC、shared-memory mapping 或 peer failure，因此不进入本表。

## 3. Payload 与 workload

固定 payload：

```text
64 B
256 B
1 KiB
4 KiB
64 KiB
1 MiB
```

每种 transport 分两种 workload：

- `transport_only`：producer 只改前 8 B sequence，consumer 只读 sequence；完整 payload 仍由 transport 传递；
- `touch_memory`：producer 写完整 payload，consumer 逐字节校验完整 payload。

前者测 ownership/synchronization/transport overhead，不能解释成“应用处理了整块 1 MiB”；后者把内存触碰成本纳入，但仍不包含业务 serialization。

## 4. JSONL schema v1

每行都是独立 JSON object，且必须带 `schema_version=1` 和 `run_id`。

### environment

只出现一次，记录 UTC、host、kernel、CPU、memory、compiler、build type、source revision、clock、methodology 和 quantile method。

### baseline_status

每个 required transport 恰好一条：

```json
{
  "type": "baseline_status",
  "schema_version": 1,
  "run_id": "...",
  "transport": "iceoryx",
  "available": false,
  "detail": "iceoryx_posh CMake package was not found; no measurement emitted"
}
```

`available=false` 时不得出现该 transport 的 `result`。显式选择不可用 baseline 时进程返回 3；默认仍测完其余真实 baseline。

### result

每个 result 除 transport/mode/access/payload/iteration/warmup 外还记录：

- `case_id` 与 `trial`；
- `status=ok`；
- completed RTT、logical message 和 payload bytes transferred；
- wall time、RTT/s、message/s、MiB/s；
- P50/P95/P99/P99.9/MAX RTT；
- user/system/total CPU、CPU utilization；
- voluntary/involuntary context switch；
- parent/child peak RSS。

约束：

```text
completed_round_trips == iterations
logical_messages == 2 * iterations
payload_bytes_transferred == logical_messages * payload_bytes
P50 <= P95 <= P99 <= P99.9 <= MAX
```

## 5. Trial 与失败语义

`--trials=N` 在相同 case 下执行 N 次独立 fork/setup/measurement，result 的 trial 从 1 开始。框架保留 raw trial，不在 C++ runner 内挑最好值或只输出平均值。

任一真实 case 出现 timeout、peer exit、payload corruption、short I/O 或 resource-metric failure，runner 输出 stderr diagnostic 并非零退出；不会生成 `status=ok`。不可用 baseline 是独立的 availability 状态，不伪装为运行时失败或数值结果。

## 6. 测试合同

benchmark smoke 必须机器校验：

- environment 1 条；
- baseline_status 5 条；
- self-test result 16 条；
- schema/run/case/trial/status 与全部要求 metric 存在；
- iceoryx 为 unavailable 且 reason 非空；
- 显式 iceoryx 选择返回 3；
- strict baseline 模式在完成可用 case 后返回 3；
- 两 trial case 产生 trial 1/2；
- quantile 与 exact-count invariant 成立。

## 7. 当前非目标

- iceoryx 的下载、vendor 或未验证适配；
- saturated streaming/多 outstanding；
- one-way clock synchronization；
- MPMC contention；
- target-hardware hard real-time 结论；
- 自动把单次 WSL2 数字写成产品性能承诺。


## 8. 最终证据

最终 Release raw matrix：

- revision：`91fdeb0a8fe7b0df4806f5093ec3530c8da54dd6`；
- 文件：[2026-08-21-unified-wsl2-gcc13-91fdeb0.jsonl](../benchmarks/results/2026-08-21-unified-wsl2-gcc13-91fdeb0.jsonl)；
- SHA-256：`c4ae0630a398384b2ac19bc131d7bb574a244704624ca125cf51efea1deabb90`；
- 记录：1 environment + 5 baseline status + 144 result；
- 结构：48 个逻辑 case，每个 3 trial，所有 result 至少 1,000 measured sample；
- 独立 jq invariant 校验：通过；
- iceoryx：**INCOMPLETE / unavailable**，无数值 result。

汇总见 [BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md)，解释见 [performance-analysis.md](performance-analysis.md)。
