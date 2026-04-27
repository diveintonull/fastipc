# FastIPC 测试矩阵

提交身份改写导致当前提交 ID 与原始日志文件名中的旧 ID 不同；对照见 [提交身份改写映射](../../COMMIT_IDENTITY_MAP.md)。原始日志未改写。
## 已验证快照

| 字段 | 值 |
| --- | --- |
| 日期 | 2026-08-20 |
| Revision | `31b21074189146447e617003330006803bbb27c14` |
| Host | WSL2 下 Ubuntu 24.04.4 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Architecture | x86_64 |
| Compiler | GNU 13.3.0 |
| Generator | Ninja 1.11.1 |
| CMake | 3.28.3 |

每种配置都使用全新 build directory，从该 revision 编译并执行：

```bash
cmake -S projects/fastipc -B BUILD -G Ninja \
  -DCMAKE_BUILD_TYPE=TYPE \
  -DFASTIPC_BUILD_TESTS=ON \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build BUILD --parallel 2
ctest --test-dir BUILD --output-on-failure
```

sanitizer 配置还为 compiler 与 executable linker 传入匹配 flag，并使用 `-fno-omit-frame-pointer`。

## 结果

| 配置 | Sanitizer option | 通过 | 失败 | 原始日志 |
| --- | --- | ---: | ---: | --- |
| Debug | 无 | 15 | 0 | [日志](tests/results/2026-08-20-debug-2bdd95f.log) |
| Release | 无 | 15 | 0 | [日志](tests/results/2026-08-20-release-2bdd95f.log) |
| ASan | `detect_leaks=1:halt_on_error=1` | 15 | 0 | [日志](tests/results/2026-08-20-asan-2bdd95f.log) |
| UBSan | `halt_on_error=1:print_stacktrace=1` | 15 | 0 | [日志](tests/results/2026-08-20-ubsan-2bdd95f.log) |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 15 | 0 | [日志](tests/results/2026-08-20-tsan-2bdd95f.log) |

完整矩阵共执行 75/75 个 CTest entry。Debug、Release、ASan、UBSan、TSan 均包含 operation-lifetime regression；该 regression 源于 AutoRuntime crash/restart integration test 在 ASan 下发现的 receive-versus-unmap race。

该 WSL2 kernel 下，TSan process tree 使用：

```bash
setarch x86_64 -R env \
  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir BUILD --output-on-failure
```

对 TSan process 禁用 address randomization，可避开 WSL shadow-memory mapping collision。native Linux CI 不用 host-specific wrapper，直接运行同一 TSan binary。

## CTest 清单

共有 15 个 registered CTest entry：

- `fastipc.transport`：一个 composite executable，包含 21 个 public-seam shared-memory behavior case；
- `fastipc.uds`：一个 composite executable，包含 13 个 Unix Domain Socket behavior/hostile-input case；
- `fastipc.benchmark_smoke`：六个短 case，覆盖三种 transport 的 64 B 与 1 MiB，并检查 JSONL schema；
- 12 个可单独寻址的 fault entry。

12 个自动化 fault entry：

1. Peer Missing
2. Producer Crash
3. Consumer Crash
4. Restart
5. Slow Consumer
6. Timeout
7. Malformed Header
8. Version Mismatch
9. Stale Shared Memory
10. Queue Full
11. Queue Empty
12. Rapid Restart

单独 fault entry 有意重跑 composite shared-memory executable 的子集。因此“15 个 CTest entry”与“21 个内部 shared-memory case”属于不同层级，不能当作独立测试相加。

第 21 个 shared-memory case `local_close_waits_for_blocked_operation` 会启动无限 blocked receive，从另一线程调用 `Close()`，验证 receive 在 mapping 释放前返回 `Closed`。它是 commit `31b2107` 的直接 regression。

## CI

[build_and_test.yml](.github/workflows/build_and_test.yml) 替代继承的上游 release/multi-platform workflow；后者引用了已移除的 `libsharedmemory` target。衍生项目只支持 Linux，当前以五个独立 Ubuntu job 运行 Debug、Release、ASan、UBSan、TSan。

## 2026-08-21 零拷贝增量验证

上面的 15 项矩阵是历史基线，原始日志保持不变。本节记录布局 2.0、借用 API 与崩溃恢复进入本地提交后的独立增量验证。

| 字段 | 值 |
| --- | --- |
| Revision | `31aaf84a78decea72b641c37797bbad200b4bebd` |
| 主机 / 内核 | WSL2 / `6.6.87.2-microsoft-standard-WSL2` |
| 编译器 | GNU 13.3.0 |
| 构建 | Ninja；Debug、Release、ASan、UBSan、TSan |
| CTest 数量 | 每种配置 22 项，其中 `fault` 标签 17 项 |

| 配置 | Sanitizer 参数 | 通过 | 失败 | 原始日志 |
| --- | --- | ---: | ---: | --- |
| Debug | 无 | 22 | 0 | [日志](tests/results/2026-08-21-debug-31aaf84.log) |
| Release | 无 | 22 | 0 | [日志](tests/results/2026-08-21-release-31aaf84.log) |
| ASan | `detect_leaks=1:halt_on_error=1` | 22 | 0 | [日志](tests/results/2026-08-21-asan-31aaf84.log) |
| UBSan | `print_stacktrace=1:halt_on_error=1` | 22 | 0 | [日志](tests/results/2026-08-21-ubsan-31aaf84.log) |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 22 | 0 | [日志](tests/results/2026-08-21-tsan-31aaf84.log) |

完整增量矩阵为 110/110。编译阶段没有产生警告，五种配置都执行：

- 原有共享内存、UDS 和 12 类基础故障测试；
- 零拷贝聚合测试与稳态无普通堆分配探针；
- producer loan crash、consumer sample crash、outstanding handle close；
- producer/consumer paused takeover；
- 四种 transport × 两种访问模式 × 两种载荷的基准冒烟测试。

复现命令：

```bash
cmake -S projects/fastipc -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=TYPE
cmake --build BUILD
ctest --test-dir BUILD --output-on-failure
```

ASan、UBSan、TSan 分别在 `CMAKE_CXX_FLAGS` 中加入对应 `-fsanitize=...` 与 `-fno-omit-frame-pointer`。本机 TSan 继续使用上文记录的 `setarch x86_64 -R` 包装；不使用该包装时 WSL2 会在测试代码运行前报告 shadow-memory mapping 冲突。

该矩阵证明已自动化路径在本环境中通过，不宣称穷举所有跨进程交错，也不代替 native Linux、ARM 或真实机器人硬件验证。


## 2026-08-21 统一 benchmark 增量验证

本节记录 schema v1、baseline availability、trial identity、精确计数与尾延迟样本下限进入最终源码后的全新构建结果。

| 字段 | 值 |
| --- | --- |
| Revision | `91fdeb0a8fe7b0df4806f5093ec3530c8da54dd6` |
| 主机 / 内核 | WSL2 / `6.6.87.2-microsoft-standard-WSL2` |
| 编译器 | GNU 13.3.0 |
| CTest | 每种配置 22 项 |
| 合计 | 110/110 |

| 配置 | Sanitizer 参数 | 通过 | 失败 | 原始日志 |
| --- | --- | ---: | ---: | --- |
| Debug | 无 | 22 | 0 | [日志](tests/results/2026-08-21-debug-91fdeb0.log) |
| Release | 无 | 22 | 0 | [日志](tests/results/2026-08-21-release-91fdeb0.log) |
| ASan | `detect_leaks=1:halt_on_error=1` | 22 | 0 | [日志](tests/results/2026-08-21-asan-91fdeb0.log) |
| UBSan | `halt_on_error=1:print_stacktrace=1` | 22 | 0 | [日志](tests/results/2026-08-21-ubsan-91fdeb0.log) |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 22 | 0 | [日志](tests/results/2026-08-21-tsan-91fdeb0.log) |

benchmark smoke 机器校验 environment/baseline/result record 数、统一 run ID、schema、case/trial/status、精确 RTT/消息/字节公式、quantile 顺序、iceoryx unavailable 无假 result、显式 unavailable exit 3、strict-mode exit 3，以及两轮 trial identity。

该 WSL2 的 GCC TSan 仍使用 `setarch x86_64 -R`；直接运行会在测试代码开始前因 shadow-memory mapping conflict 退出。最终 TSan 日志来自 wrapper 下的真实 22/22，不包含 suppression。

## 2026-08-21 有界 MPMC 增量验证

本节记录独立 MPMC layout、CAS reservation、per-slot sequence、futex epoch、跨线程/跨进程 exact-once、abandoned reservation 限制刻画与 contention runner 进入本地提交后的完整矩阵。

| 字段 | 值 |
| --- | --- |
| Revision | `6d9c7549666b6c064cce75dd6c4f19e34044dbfd` |
| 主机 / 内核 | WSL2 / `6.6.87.2-microsoft-standard-WSL2` |
| 编译器 | GNU 13.3.0 |
| CTest | 每种配置 27 项；`fault` 标签 18 项，`benchmark` 标签 2 项 |
| 合计 | 135/135 |

| 配置 | Sanitizer 参数 | 通过 | 失败 | 原始日志 | SHA-256 |
| --- | --- | ---: | ---: | --- | --- |
| Debug | 无 | 27 | 0 | [日志](tests/results/2026-08-21-debug-6d9c754.log) | `8460368f0dfb5006e5b2d188c630dc7d934d2768c329cc5d87a76f77751ef696` |
| Release | 无 | 27 | 0 | [日志](tests/results/2026-08-21-release-6d9c754.log) | `ec7338a8d3e734fd1069c16d8dcdcbc82c05dfdfb82cac87854e61d3180f8f48` |
| ASan | `halt_on_error=1:detect_leaks=1` | 27 | 0 | [日志](tests/results/2026-08-21-asan-6d9c754.log) | `032a0e22f09846b30454454517d8ece5b6e25fb0ce0a233500991984582f9593` |
| UBSan | `halt_on_error=1:print_stacktrace=1` | 27 | 0 | [日志](tests/results/2026-08-21-ubsan-6d9c754.log) | `6cba800930f4613be761a2e0be9e70b9182909d17b00d2e2ecb34d6f660e20c6` |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 27 | 0 | [日志](tests/results/2026-08-21-tsan-6d9c754.log) | `7df560eb8e8c5727471f947e1d31c217b4a6e5076ced33bb442b83a8e48d07e3` |

相对上一轮 22 项矩阵新增 5 个 registered entry：

1. `fastipc.mpmc`：聚合执行参数、FIFO、背压、futex、线程、进程与故障刻画；
2. `fastipc.mpmc.threaded_exactly_once`：4P4C × 每 producer 2,000 条；
3. `fastipc.mpmc.process_exactly_once`：3P3C × 每 producer 500 条、独立 `Open` mapping；
4. `fastipc.mpmc.abandoned_reservation`：producer 只推进 enqueue cursor 后退出，后续消息可 publish，但 FIFO consumer 超时；
5. `fastipc.mpmc.benchmark_smoke`：JSON schema、统一 run ID、1P1C/2P2C、exact-count、quantile order 和两 trial identity。

五种配置都运行完整 27 项，而不是只跑 MPMC filter。日志机器检查结果：

- 每份均包含 `100% tests passed, 0 tests failed out of 27`；
- ASan/LSan 无 error 或 leak report；
- UBSan 无 runtime error；
- TSan 无 warning/summary，且没有 suppression；
- Debug、Release 与三类 sanitizer 合计 135/135。

TSan 在本机继续用 `setarch x86_64 -R` 避免 WSL2 shadow-memory mapping 冲突；这是运行环境包装，不是数据竞争 suppression。

`abandoned_reservation` 是失败语义表征：它证明当前会产生队首阻塞，也证明实现没有假装跳过或回收未发布 slot。它不证明 crash recovery；完整 robust recovery 仍为 `INCOMPLETE`。

## 2026-08-21 Seeded Chaos / Soak 增量验证

本节先记录 revision `906d0f2d88fb4af504377a4f35f69381ca773366` 上已经终结的构建矩阵和固定操作数短跑。30 分钟时长运行只有在进程正常退出、summary 通过且完整原始证据封存后才会在下文标为完成。

| 字段 | 值 |
| --- | --- |
| 主机 / 内核 | WSL2 / `6.6.87.2-microsoft-standard-WSL2` |
| 编译器 | GNU 13.3.0 |
| 构建 | fresh Ninja；Debug、Release、ASan、UBSan、TSan |
| CTest | 每种配置 30 项；`fault` 标签 20 项，`chaos` 标签 3 项 |
| 合计 | 150/150 |

| 配置 | Sanitizer 参数 | 通过 | 失败 | 原始日志 | SHA-256 |
| --- | --- | ---: | ---: | --- | --- |
| Debug | 无 | 30 | 0 | [日志](tests/results/2026-08-21-debug-906d0f2.log) | `0017ac624353f03f43995ca8948bd07010a16c279149dc21bb320baf9131ed6d` |
| Release | 无 | 30 | 0 | [日志](tests/results/2026-08-21-release-906d0f2.log) | `84b781a752f1238aff17ead163ad0a2c23366a93bc4b526ad9f3b3a25c2ef517` |
| ASan | `detect_leaks=1:halt_on_error=1` | 30 | 0 | [日志](tests/results/2026-08-21-asan-906d0f2.log) | `27bc4c4efee034147a838627ce685b126a7df83fa87e9731007442f290894793` |
| UBSan | `halt_on_error=1:print_stacktrace=1` | 30 | 0 | [日志](tests/results/2026-08-21-ubsan-906d0f2.log) | `635ec6ea951d3f21f8a1456d71eb987cf544d4ef21d66358cbc5d3954380c1f5` |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 30 | 0 | [日志](tests/results/2026-08-21-tsan-906d0f2.log) | `449f76be2575ce28d96bf76b4807e88cab57c97051548f032c04c68579a6cef5` |

五份日志均恰好包含一次 `100% tests passed, 0 tests failed out of 30`。额外扫描未发现编译 warning、FAILED、断言失败、ASan/LSan、UBSan runtime error 或 TSan data-race 诊断。TSan 继续使用 `setarch x86_64 -R` 规避该 WSL2 的 shadow-memory mapping collision，不使用 suppression。

### 固定 seed 的可复现短跑

执行参数：

```bash
fastipc_chaos_runner \
  --seed 20260821 \
  --operations 256 \
  --payload 4096 \
  --slot-count 8 \
  --peer-timeout-ms 50 \
  --command-timeout-ms 2500 \
  --delay-ms 20 \
  --latency-window 64
```

同一 revision 和参数独立执行两次；从两份 JSONL 提取 256 个 `record_type=operation` 的操作名后，`cmp` 退出码为 0。非空操作序列 SHA-256 为：

```text
ed38acba51ce0aa4c58526fd78344d4d9d9cf172dc9c2bfa33d05fb1d9d6cc8a
```

八类操作各出现 32 次。canonical [JSONL](tests/results/2026-08-21-chaos-seed-20260821-ops-256-906d0f2.jsonl) 的 SHA-256 为 `fc1c46fef156a914836cf1119fb31623e9d12c80ff0cef1f4e80f54fa5c2c873`，summary 为：

| 指标 | 结果 |
| --- | ---: |
| operation / crash / restart / recovery | 256 / 64 / 128 / 128 |
| checksum / lost / duplicate / unexpected message | 0 / 0 / 0 / 0 |
| expected / unexpected timeout | 64 / 0 |
| expected drop / operation failure / cleanup failure | 32 / 0 / 0 |
| maximum outstanding / retained plan | 9 / 8 |
| probe / retained probe | 256 / 128 |
| baseline / final P99 / drift | 185.391 / 215.030 / 29.639 µs |
| RSS start / end / maximum / growth | 9,416 / 9,216 / 9,608 / -200 KiB |
| status / duration | passed / 2,743.441 ms |

该短跑只证明固定 seed 操作计划可复现和自动化不变量通过，不等价于长时稳定性。

### 30 分钟真实 soak

固定 revision 的 fresh Release binary 执行：

```bash
fastipc_chaos_runner \
  --seed 20260821 \
  --operations 0 \
  --duration-ms 1800000 \
  --payload 4096 \
  --slot-count 8 \
  --peer-timeout-ms 50 \
  --command-timeout-ms 2500 \
  --delay-ms 20 \
  --latency-window 4096 \
  --max-memory-growth-kib 65536 \
  --max-p99-drift-us 5000
```

进程自然退出且退出码为 0。完整逐操作证据以无时间戳 gzip 保存为 [JSONL.gz](tests/results/2026-08-21-chaos-seed-20260821-30min-906d0f2.jsonl.gz)，stderr 为 [0 字节日志](tests/results/2026-08-21-chaos-seed-20260821-30min-906d0f2.stderr.log)。

| 指标 | 结果 |
| --- | ---: |
| operation / crash / restart / recovery | 168,954 / 42,238 / 84,476 / 84,476 |
| checksum / lost / duplicate / unexpected message | 0 / 0 / 0 / 0 |
| expected / unexpected timeout | 42,239 / 0 |
| expected drop / operation failure / cleanup failure | 21,119 / 0 / 0 |
| maximum outstanding / retained plan | 9 / 8 |
| probe / retained probe | 168,954 / 8,192 |
| baseline / final P99 / drift | 189.500 / 186.844 / -2.656 µs |
| RSS start / end / maximum / growth | 9,608 / 9,816 / 9,824 / 208 KiB |
| status / duration | passed / 1,800,004.052 ms |
| 64 MiB RSS / 5 ms P99 门槛 | 均未超过 |

静止文件上的机器校验结果：

- 168,956 行恰好由 1 条 environment、168,954 条 operation 和 1 条 summary 构成；
- `status=ok` 恰好 168,954 条，`status=error` 为 0；
- stderr 为 0 字节；
- runner 父进程在 69,154 和 163,939 次操作附近均为 9 个 FD，未随 actor 重启次数增长；
- 原始 JSONL 为 69,633,384 字节，SHA-256 为 `51e70f47b663a8db01b3972578cb5f6137c9210e6a2495cb9adf8038b5672728`；
- gzip 为 2,305,656 字节，SHA-256 为 `356107ea33e8c660d7f148988e76ce992c2276696d196da5d754dc5db8767c58`；
- `gzip -t` 通过，`gzip -dc | sha256sum` 重新得到原始 SHA-256。

[SHA-256 清单](tests/results/2026-08-21-chaos-evidence-906d0f2.sha256) 可直接在 `tests/results` 目录用 `sha256sum -c` 复核。

| 阶段 | 状态 | 边界 |
| --- | --- | --- |
| 30 分钟 | **PASSED** | 上述固定 revision、seed、命令、JSONL、summary 和退出码均已保存 |
| 2 小时 | **INCOMPLETE** | 未运行，不从 30 分钟结果外推 |
| overnight（8 小时） | **INCOMPLETE** | 未运行 |
| 24 小时 | **INCOMPLETE** | 只在前述阶段稳定后考虑 |
