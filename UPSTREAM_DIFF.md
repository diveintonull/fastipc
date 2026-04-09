# FastIPC upstream-to-derivative boundary

## Purpose

This document separates inherited history from current FastIPC engineering. It
is an authorship and maintenance map, not a claim that every idea listed below
is novel in the research sense.

## Provenance and legal boundary

- Primary upstream: [kyr0/libsharedmemory](https://github.com/kyr0/libsharedmemory)
- Pinned upstream commit: `9e24caaefb28826e99a33be2dd1350725558dd80`
- Local subtree import commit: `e8d6d3c`
- Imported tree: `b18d57de96feeaca5a9048c47e4ed99cb118edba`
- License: MIT; the upstream copyright remains verbatim in `LICENSE`.
- Additional retained notice: `test/lest.hpp` carries the Boost Software
  License 1.0 and Martin Moene's notice.

The upstream commits were imported as history, not squashed or redated. The
workspace-level `UPSTREAMS.md` records both derivative projects and tag
namespaces. No code from the two secondary FastIPC references was vendored.

## What the pinned upstream supplied

The imported baseline supplied a compact C++20 repository, a named-memory RAII
concept, last-value streams, a fixed-capacity queue, examples and FFI bindings,
one CTest target, CI and packaging scaffolding. Its compiled logic lived almost
entirely in `include/libsharedmemory/libsharedmemory.hpp`.

The upstream queue used host-native metadata, shared producer/consumer spin
locks and a shared count. It had no protocol magic/version/total size,
initialization election, endpoint identity, generation, heartbeat, futex wait,
absolute deadline, typed transport seam, UDS adapter, fault matrix, percentile
benchmark, sanitizer matrix or profiling evidence. The detailed baseline audit
is in `docs/upstream-analysis.md`.

## Current build boundary

`CMakeLists.txt` now exposes only:

- `FastIPC::fastipc`, built from `src/shared_memory_transport.cpp`,
  `src/unix_domain_socket_transport.cpp`, and `src/status.cpp`;
- the two current behavior/fault test executables;
- the current cross-process benchmark executable.

The imported `include/libsharedmemory`, `example`, `ffi`, `test`,
screenshot and legacy changelog remain physically present for historical
inspection. They are not included or linked by the current build, and their
features are not claimed as FastIPC behavior. Broad deletion was intentionally
not performed without a separate destructive-file approval.

## Keep, rewrite, add

### Kept with attribution

| Item | Current disposition | Evidence |
| --- | --- | --- |
| MIT license and copyright | Retained verbatim | `LICENSE` |
| Full upstream development history | Retained through subtree import | `git log -- projects/fastipc`, import `e8d6d3c` |
| Named-memory RAII and bounded-capacity concepts | Used as the audited starting point | `docs/upstream-analysis.md` |
| Small CMake/CTest repository shape | Reworked into the current target graph | `CMakeLists.txt` |
| Legacy source and examples | Historical only, excluded from build | current `CMakeLists.txt` |

### Rewritten

| Surface | Upstream | FastIPC | Evidence |
| --- | --- | --- | --- |
| Public API | Concrete stream/queue classes | Typed `Transport`, `Status`, `Result`, `Deadline`, and two adapters | `include/fastipc/` |
| Mapping lifecycle | unlink-before-create and host-specific wrapper | Linux creator/open validation, mode 0600 default, size checks and initialization election | `src/shared_memory_transport.cpp`, creation/open sections |
| Shared layout | Host-native fields without identity/version | Magic, 1.1 version, endian marker, byte sizes, generation, endpoints, epochs and isolated cursors | `src/shared_memory_layout.hpp` |
| Queue algorithm | Lock-serialized sides plus shared count | SPSC single-writer head/tail publication without data-plane CAS | `Send` and `Receive` |
| Memory ordering | Partial atomic use without cross-process audit | Per-field release/acquire proof and lock-free width assertions | `docs/memory-model.md` |
| Waiting | Yield/spin or immediate false | Bounded active spin, epoch futex wait, predicate recheck and monotonic absolute deadlines | futex helpers, `Send`, `Receive` |
| Backpressure | Immediate boolean failure | `Block`, deadline-bounded `Timeout`, immediate `Drop`, typed counters | `transport.hpp`, `Send` |
| Lifecycle errors | Small enum plus exceptions | Typed setup and operation statuses, native errno where useful | `status.hpp`, `status.cpp` |
| Repository front door | Upstream feature/readme and setup Makefile | Current capabilities, evidence links, safe build commands and limitations | `README.md`, `Makefile`, `SECURITY.md` |

### Added by the derivative work

| Capability | Design responsibility | Evidence |
| --- | --- | --- |
| Role ownership and fencing | PID plus Linux start ticks plus per-claim token; producer restart advances generation | `EndpointMetadata`, claim/open paths, `OwnsRole` |
| Health and recovery | Dedicated heartbeat thread, amortized identity probes, peer-death statuses, reclaim under `flock` | heartbeat/liveness helpers and restart tests |
| UDS transport | `AF_UNIX` `SOCK_SEQPACKET`; inline frames and sealed memfd plus `SCM_RIGHTS` for large payloads | `unix_domain_socket_transport.cpp` |
| Hostile-input validation | Wire magic/version/flags/length validation and required memfd seals | UDS receive path and hostile-input tests |
| Fault automation | Twelve named cross-process shared-memory fault cases plus UDS disconnect/corruption cases | `CMakeLists.txt`, `tests/` and `docs/fault-matrix.md` |
| Benchmark | Shared memory, UDS and pipe; 64 B to 1 MiB; throughput, P50/P95/P99, CPU, context switches and memory | `benchmarks/`, `BENCHMARK_RESULTS.md` |
| Performance engineering | Raw non-root perf evidence, rejected probe-only result, bounded-spin experiment and heartbeat ownership experiment | `benchmarks/profiling/` |
| Verification matrix | Debug, Release, ASan, UBSan and TSan automation with revision-pinned raw logs | `.github/workflows/build_and_test.yml`, `TEST_MATRIX.md` |
| Design and code evidence | Memory-model proof, module guides and code-anchored evidence | `docs/` |

## Commit-level evidence

The derivative work is intentionally incremental. Important commits are:

| Commit | Boundary introduced |
| --- | --- |
| `41882e3` | Versioned SPSC shared-memory transport and public seam |
| `a547a64` | Futex epoch protocol and absolute deadlines |
| `f32957c` | Generation-aware producer restart |
| `ac63a80` | Role recovery after process death |
| `485b05f` | Heartbeat leases and stale-role fencing |
| `1d1d4cc` | Automated twelve-case FastIPC fault matrix |
| `a5920c6` | Unix-domain-socket transport and large-message descriptor path |
| `c5cb58a` | Cross-process benchmark harness |
| `0efab39` through `866d32b` | Baseline profiling and two measured optimizations |
| `8c542ca` | Five-configuration build/sanitizer verification |

At revision `8c542ca`, the reproducible command
`git diff --stat e8d6d3c:projects/fastipc 8c542ca:projects/fastipc` reports 49
changed files, 7,806 insertions and 161 deletions. That number is a review aid,
not an authorship percentage: generated measurement records and documentation
are included, while retained historical files are outside the diff.

## Secondary references

- `vt-tv/lockfree_ipc_ringbuffer` was used to compare layout validation,
  sequence/futex choices and multiprocess test taxonomy.
- `rigtorp/ipc-bench` was used to compare benchmark organization and transport
  baselines.

No source from either repository is present in the compiled core. If later work
copies or adapts code rather than ideas or behavior, its exact license and
attribution must be added before distribution.

## Deliberate non-claims

FastIPC does not claim MPSC/MPMC, portable ISO C++ process-shared atomics,
public zero-copy loan ownership, authentication/encryption, hard real-time
latency, crash-proof mid-instruction recovery, production security review, or
native-hardware performance based on the recorded WSL2 results. These are
explicit future design problems rather than implied capabilities.
