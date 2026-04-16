# FastIPC test matrix

## Verified snapshot

| Field | Value |
| --- | --- |
| Date | 2026-08-20 |
| Revision | `2bdd95f00dbb95e129244a1651d0800e01ddd03b` |
| Host | Ubuntu 24.04.4 under WSL2 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Architecture | x86_64 |
| Compiler | GNU 13.3.0 |
| Generator | Ninja 1.11.1 |
| CMake | 3.28.3 |

Every configuration was created in a new build directory, compiled from this
revision, and run with:

```bash
cmake -S projects/fastipc -B BUILD -G Ninja \
  -DCMAKE_BUILD_TYPE=TYPE \
  -DFASTIPC_BUILD_TESTS=ON \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build BUILD --parallel 2
ctest --test-dir BUILD --output-on-failure
```

Sanitizer configurations additionally passed matching compiler and executable
linker flags with `-fno-omit-frame-pointer`.

## Results

| Configuration | Sanitizer options | Passed | Failed | Raw log |
| --- | --- | ---: | ---: | --- |
| Debug | none | 15 | 0 | [log](tests/results/2026-08-20-debug-2bdd95f.log) |
| Release | none | 15 | 0 | [log](tests/results/2026-08-20-release-2bdd95f.log) |
| ASan | `detect_leaks=1:halt_on_error=1` | 15 | 0 | [log](tests/results/2026-08-20-asan-2bdd95f.log) |
| UBSan | `halt_on_error=1:print_stacktrace=1` | 15 | 0 | [log](tests/results/2026-08-20-ubsan-2bdd95f.log) |
| TSan | `halt_on_error=1:second_deadlock_stack=1` | 15 | 0 | [log](tests/results/2026-08-20-tsan-2bdd95f.log) |

The required matrix therefore executed 75/75 CTest entries successfully.
Debug, Release, ASan, UBSan, and TSan all include the operation-lifetime
regression added after ASan exposed a receive-versus-unmap race in the
AutoRuntime crash/restart integration test.

On this WSL2 kernel, the TSan process tree was launched with:

```bash
setarch x86_64 -R env \
  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir BUILD --output-on-failure
```

Disabling address randomization for the TSan process avoids WSL shadow-memory
mapping collisions. Native Linux CI runs the same TSan binary without this
host-specific wrapper.

## CTest inventory

There are 15 registered CTest entries:

- `fastipc.transport`: one composite executable containing 21 public-seam
  shared-memory behavior cases;
- `fastipc.uds`: one composite executable containing 13 Unix-domain-socket
  behavior and hostile-input cases;
- `fastipc.benchmark_smoke`: six short cases covering three transports at
  64 B and 1 MiB, plus JSONL schema assertions;
- 12 separately addressable fault entries.

The 12 automated fault entries are:

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

The separately addressable fault entries intentionally rerun a subset of the
composite shared-memory executable. Therefore "15 CTest entries" and "21
internal shared-memory cases" describe different layers and must not be added
as if they were unique tests.

The 21st shared-memory case, `local_close_waits_for_blocked_operation`, starts
an infinite blocked receive, calls `Close()` from another thread, and verifies
that the receive returns `Closed` before the mapping is released. It is the
direct regression for commit `2bdd95f`.

## CI

[build_and_test.yml](.github/workflows/build_and_test.yml) replaces the inherited
upstream release/multi-platform workflow, which referenced removed
`libsharedmemory` targets. The derivative project is Linux-specific and now
runs Debug, Release, ASan, UBSan, and TSan as five independent Ubuntu jobs.
