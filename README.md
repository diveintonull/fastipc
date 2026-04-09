# FastIPC

FastIPC is a Linux C++20 IPC library built as a deep, attributed derivative of
`kyr0/libsharedmemory`. The current core provides a versioned SPSC
shared-memory channel, futex blocking with absolute deadlines, peer
liveness/restart fencing, explicit backpressure, and a common transport seam
implemented by shared memory and Unix domain sockets.

This project has not undergone a production security review and is not a
replacement for iceoryx, eCAL, or a security-reviewed IPC stack.

## Capabilities

- Linux POSIX shared-memory mapping with creator/open validation and mode 0600
  defaults;
- versioned, endian-marked layout with total size, initialization state, and
  generation;
- cache-line-isolated SPSC head/tail cursors and fixed-capacity slots;
- audited acquire/release publication without `memory_order_seq_cst`;
- futex epochs, recheck-before-sleep, monotonic absolute deadlines, and bounded
  active spin;
- PID, Linux process-start ticks, role token, heartbeat lease, and generation
  fencing;
- producer/consumer crash detection, stale segment recovery, and restart flow;
- Block, Timeout, and Drop backpressure with typed `Status` values and
  counters;
- `Transport` API with `SharedMemoryTransport` and
  `UnixDomainSocketTransport`;
- UDS `SOCK_SEQPACKET` inline frames through 64 KiB and sealed-memfd
  descriptor transfer for larger payloads;
- cross-process benchmark matrix for 64 B through 1 MiB, with pipe as a third
  baseline;
- automated Debug, Release, ASan, UBSan, and TSan coverage.

## Architecture

```text
Application
    |
    v
Transport { Send, Receive, Stats, Close }
    |
    +-- SharedMemoryTransport
    |      +-- versioned mapped layout
    |      +-- bounded SPSC ring
    |      +-- active spin -> futex epoch wait
    |      +-- generation / role / heartbeat control plane
    |
    +-- UnixDomainSocketTransport
           +-- AF_UNIX SOCK_SEQPACKET
           +-- inline frame or sealed memfd + SCM_RIGHTS
```

Shared memory uses two ownership planes:

- data plane: one producer owns `head` and slot writes; one consumer owns
  `tail` and slot reads;
- control plane: role claims, generation, PID/start-ticks identity, heartbeat,
  and cleanup.

The heartbeat jthread exclusively refreshes the lease timestamp. Successful
message operations increment an independent progress sequence.

## Build and test

From this directory:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTIPC_BUILD_TESTS=ON \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The default build produces:

- `FastIPC::fastipc`, the static library target;
- `fastipc_tests`, the shared-memory behavior/fault executable;
- `fastipc_uds_tests`, the Unix-socket behavior/hostile-input executable;
- `fastipc_benchmark`, the JSONL benchmark runner.

## Shared-memory example

```cpp
#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;

fastipc::ChannelConfig config;
config.name = "sensor_frames";
config.slot_count = 64;
config.max_message_size = 4096;
config.unlink_on_owner_close = true;

auto producer_result =
    fastipc::SharedMemoryTransport::CreateProducer(config);
auto consumer_result =
    fastipc::SharedMemoryTransport::OpenConsumer(config);
if (!producer_result || !consumer_result) {
  // Inspect result.status(); setup failures are typed.
  return;
}

auto producer = std::move(producer_result).take_value();
auto consumer = std::move(consumer_result).take_value();

const std::array<std::byte, 3> payload{
    std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
auto sent = producer->Send(
    payload,
    fastipc::SendOptions{
        fastipc::BackpressurePolicy::Block,
        fastipc::Deadline::After(100ms)});

std::array<std::byte, 4096> destination{};
auto received =
    consumer->Receive(destination, fastipc::Deadline::After(100ms));
```

Producer and consumer normally live in separate processes. The example places
them together only to show the public seam.

## Backpressure and failure surface

`SendOptions` selects:

| Policy | Full queue behavior |
| --- | --- |
| `Block` | bounded active spin, then futex sleep until space, close, peer failure, or deadline |
| `Timeout` | same bounded wait mechanics; deadline returns `Timeout` |
| `Drop` | immediate `Dropped` without unbounded allocation |

Other typed outcomes include `Closed`, `PeerUnavailable`, `PeerDead`,
`StaleGeneration`, `RoleConflict`, `LayoutMismatch`,
`MessageTooLarge`, `BufferTooSmall`, and `CorruptData`.

## Evidence

- [Benchmark methodology](docs/benchmark-methodology.md)
- [Raw benchmark and full result table](BENCHMARK_RESULTS.md)
- [Two perf experiments with raw stat/record/report evidence](benchmarks/profiling/README.md)
- [Five-configuration test matrix and raw CTest logs](TEST_MATRIX.md)
- [Fault matrix](docs/fault-matrix.md)
- [Memory-order audit](docs/memory-model.md)
- [Upstream audit](docs/upstream-analysis.md)
- [Exact upstream-to-derivative boundary](UPSTREAM_DIFF.md)

The benchmark report is revision-pinned. Results are not copied from upstream
and are not generalized beyond their recorded WSL2 host and synchronous
ping-pong protocol.

## Upstream and Attribution

Primary upstream:

- [kyr0/libsharedmemory](https://github.com/kyr0/libsharedmemory)
- pinned commit: `9e24caaefb28826e99a33be2dd1350725558dd80`
- license: MIT
- upstream copyright is preserved verbatim in [LICENSE](LICENSE)
- original commits were imported with history, not squashed or redated

What remains attributable to upstream:

- the historical repository tree and development lineage;
- the original MIT license and notices;
- the initial small CMake/CTest scaffold and named-memory/fixed-capacity design
  concepts used as the audit baseline.

What FastIPC rewrote or newly implemented:

- the compiled public API, layout, mapping lifecycle, queue algorithm,
  synchronization, liveness/recovery, transport adapters, status model,
  benchmark, fault tests, sanitizer matrix, profiling, and design and implementation documentation.

Secondary repositories were design references only:

- `vt-tv/lockfree_ipc_ringbuffer` for comparison with sequence/futex protocol
  choices;
- `rigtorp/ipc-bench` for benchmark taxonomy.

No source from those secondary repositories is vendored into the compiled
FastIPC core.

The imported upstream examples, FFI bindings, old tests, changelog, screenshot,
and single-header implementation remain in Git history and, where still
present in this workspace tree, are historical artifacts only. They are not
reachable from the current CMake build and their capabilities are not claimed
as FastIPC features. Broad physical deletion was intentionally not performed
without a separate destructive-file approval.

See [UPSTREAM_DIFF.md](UPSTREAM_DIFF.md) for the exact retained/rewritten/new
boundary.

## Limitations

- SPSC only; MPSC/MPMC requires a different reservation and recovery proof.
- Payloads are copied into and out of fixed-size slots; there is no public
  zero-copy loan lifetime.
- Shared-memory peers must use the same Linux ABI and compatible FastIPC layout.
- Heartbeats provide bounded suspicion, not a proof that a paused process is
  permanently dead.
- Recovery fences operations at API boundaries; a process frozen after its
  final ownership check would need generation-tagged slots/cursors for strict
  mid-operation fencing.
- No authentication, encryption, namespace broker, SELinux policy integration,
  NUMA placement, or real-time scheduling guarantee.
- UDS sealed-memfd transfer is descriptor-assisted shared memory, not pure
  socket-copy throughput.
- WSL2 measurements must be repeated on native Linux and target hardware before
  deployment decisions.
