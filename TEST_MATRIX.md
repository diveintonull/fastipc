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
