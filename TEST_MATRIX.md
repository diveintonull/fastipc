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
