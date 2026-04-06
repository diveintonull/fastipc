# FastIPC benchmark methodology

## Purpose

The benchmark compares the two FastIPC transports with a Linux pipe baseline
under one reproducible cross-process protocol. It reports measurements; it does
not claim that one transport is universally faster or suitable for every
workload.

## Required matrix

Every recorded full run covers:

| Transport | Mode |
| --- | --- |
| `shared_memory` | Two unidirectional SPSC rings with futex epoch wakeups |
| `unix_domain_socket` | `SOCK_SEQPACKET`; inline data through 64 KiB, sealed `memfd` plus `SCM_RIGHTS` above 64 KiB |
| `pipe` | Two nonblocking pipes, one per direction |

Payload sizes are 64 B, 256 B, 1 KiB, 4 KiB, 64 KiB, and 1 MiB.

## Protocol

Each case forks exactly one child. The parent sends one payload and waits for
the child to echo it before issuing the next payload. The first eight bytes
carry a sequence number and the final payload is compared byte-for-byte.

This is a single-outstanding ping-pong test:

1. It measures round-trip time (RTT), not one-way latency.
2. It exercises process wakeup, transport send/receive, and echo work in both
   directions.
3. It does not measure a saturated multi-producer stream.
4. Throughput is derived from completed RTTs. One RTT counts as two logical
   messages and transfers two payloads.

Shared memory uses separate request and reply channel objects because each ring
is intentionally SPSC and unidirectional. The socket transport is full duplex.
The pipe baseline uses one kernel pipe in each direction.

## Iterations and warmup

The default measured iteration count targets approximately 64 MiB of total
bidirectional payload:

```text
iterations = clamp(
    64 MiB / (2 * payload_bytes),
    100,
    20,000)
```

Warmup is excluded from every metric:

```text
warmup = clamp(iterations / 20, 5, 100)
```

The command line can override both values. `--self-test` deliberately uses
only three measured and one warmup iteration at 64 B and 1 MiB; it validates
the harness and schema, not performance.

## Timing and quantiles

The parent records each RTT with `std::chrono::steady_clock`. P50, P95, and
P99 are nearest-rank quantiles over measured iterations. Wall time surrounds
only the measured parent loop.

Reported rates are:

```text
round_trips_per_second = iterations / wall_seconds
messages_per_second    = 2 * iterations / wall_seconds
payload_mib_per_second = 2 * iterations * payload_bytes
                         / (MiB * wall_seconds)
```

## CPU, context switches, and memory

Parent and child call `getrusage(RUSAGE_SELF)` immediately before and after
their measured loops. The runner sums their deltas for:

- user CPU time;
- system CPU time;
- voluntary context switches;
- involuntary context switches.

CPU utilization is summed CPU time divided by parent wall time. It can exceed
100 percent because two processes can run concurrently.

`parent_peak_rss_kib` and `child_peak_rss_kib` are Linux `ru_maxrss`
values. They are process-lifetime peaks, not measured-window deltas, and include
the runner, allocator, and transport setup. They are useful as coarse memory
signals, not precise per-message allocation counts.

## UDS large-message interpretation

Messages at or below 64 KiB are sent inline in one `SOCK_SEQPACKET` record.
Larger messages, including the 1 MiB case, are copied into a sealed anonymous
`memfd`; the descriptor is transferred with `SCM_RIGHTS`, and the receiver
validates type, exact size, and required seals before mapping it.

Results label these paths `seqpacket_inline` and
`seqpacket_sealed_memfd`. The latter is descriptor-assisted shared memory and
must not be described as pure socket-copy throughput.

## Environment record

The first JSONL record captures UTC time, hostname, kernel, architecture, CPU
model, online logical CPU count, page size, total memory, compiler, build type,
and the source revision embedded at CMake configure time. Each following line
is one result record.

For comparable evidence:

1. configure and build Release after the benchmark commit exists, so the
   embedded revision names the measured source;
2. run the full matrix from an otherwise idle machine;
3. preserve raw JSONL rather than copying terminal summaries;
4. repeat runs when drawing performance conclusions;
5. record virtualization, power-management, and CPU-affinity conditions.

WSL2 results demonstrate behavior on that recorded host. They are not a
substitute for native Linux and target-hardware measurements.

## Commands

```bash
cmake -S projects/fastipc -B projects/fastipc/build-release \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFASTIPC_BUILD_TESTS=ON -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build projects/fastipc/build-release --target fastipc_benchmark
projects/fastipc/build-release/fastipc_benchmark
```

Use `--help` for transport, payload, iteration, and warmup filters. Standard
output is JSONL; diagnostics and typed failures go to standard error.
