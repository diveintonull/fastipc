# Shared-memory profiling and optimization experiments

This directory preserves raw evidence for the required `perf stat`,
`perf record`, and `perf report` workflow. Performance claims below are
limited to the recorded WSL2 host and workload.

## Profiling environment

| Field | Value |
| --- | --- |
| Date | 2026-08-20 |
| Baseline source | `6da1a9e4ad0c` |
| Compiler | GNU 13.3.0 |
| Profile build | `-O2 -g -fno-omit-frame-pointer` |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| perf | 6.8.12 from Ubuntu `linux-tools-6.8.0-138`, extracted under `/tmp` without root |
| `perf_event_paranoid` | 2 |
| Workload | shared memory, 64 B, 200,000 measured RTTs, 1,000 warmup RTTs |

WSL2 exposed software events only. Hardware `cycles`, `instructions`,
`cache-references`, and `cache-misses` reported `not supported`.
`perf stat` also forced user-only events, so its context-switch count was zero;
the benchmark's parent-plus-child `getrusage` deltas remain the context-switch
evidence. These limitations are evidence, not values to replace or estimate.

## Reproduction commands

The profiling build was configured as:

```bash
cmake -S projects/fastipc \
  -B /tmp/fastipc-profile-baseline-6da1a9e \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer" \
  -DFASTIPC_BUILD_TESTS=OFF \
  -DFASTIPC_BUILD_BENCHMARKS=ON
cmake --build /tmp/fastipc-profile-baseline-6da1a9e \
  --target fastipc_benchmark
```

The command represented by `PERF` was the extracted 6.8.12 binary with the
extracted `libtraceevent.so.1` on `LD_LIBRARY_PATH`.

```bash
PERF stat -r 5 -x, \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o baseline-6da1a9e-stat.csv -- \
  fastipc_benchmark --transport=shared_memory --payload=64 \
  --iterations=200000 --warmup=1000

PERF record -e cpu-clock -F 499 -g \
  -o baseline-6da1a9e.data -- \
  fastipc_benchmark --transport=shared_memory --payload=64 \
  --iterations=200000 --warmup=1000

PERF report --stdio --no-children --sort symbol \
  --percent-limit 0.5 --call-graph none \
  -i baseline-6da1a9e.data
```

The host-specific binary `perf.data` is not committed. Its rendered report,
the benchmark output from the record run, five `stat` repetitions, and the
`stat` CSV are committed. Perf table padding was stripped from the text report;
numeric and symbol content is unchanged.

## Baseline evidence

Files:

- [baseline-6da1a9e-stat.csv](baseline-6da1a9e-stat.csv)
- [baseline-6da1a9e-runs.jsonl](baseline-6da1a9e-runs.jsonl)
- [baseline-6da1a9e-record-run.jsonl](baseline-6da1a9e-record-run.jsonl)
- [baseline-6da1a9e-report.txt](baseline-6da1a9e-report.txt)

The median-throughput repetition reported:

| Metric | Baseline |
| --- | ---: |
| Logical messages/s | 153,825.684 |
| Wall time | 2,600.346 ms |
| P50 RTT | 4.074 us |
| P95 RTT | 34.345 us |
| P99 RTT | 54.549 us |
| Summed CPU time | 2,799.776 ms |
| Voluntary context switches | 161,403 |

`perf stat -r 5` reported 2,654.07 ms task-clock with 0.43 percent variation
and 635 page faults with 0.04 percent variation.

The flat 415-sample report showed:

| Symbol | Self overhead |
| --- | ---: |
| `SharedMemoryTransport::Send` | 10.12% |
| string extraction used by `/proc/<pid>/stat` parsing | 8.43% |
| `SharedMemoryTransport::Receive` | 7.71% |
| `ProcessStartTicks` | 5.54% |
| `__memmove_avx_unaligned_erms` | 5.30% |
| iostream sentry used by process-stat parsing | 4.82% |
| `__vdso_clock_gettime` | 4.58% |
| `syscall` | 4.58% |

The call-graph report used during diagnosis showed the string/iostream work
under `ProcessStartTicks -> InspectEndpoint -> PeerStatus -> Receive`.
The clock samples included both benchmark timing and transport Send/Receive.

## Experiment hypotheses

### Experiment 1  amortize process identity probes

The queue-empty/full path validated PID reuse by reopening and parsing
`/proc/<pid>/stat` before every futex wait. That is correct but unnecessarily
expensive while a fresh heartbeat lease is present.

The intended change is to retain the full PID/start-ticks check at setup, after
lease expiry, and at a bounded probe interval. Futex waits must also use that
bounded interval so an infinite caller deadline cannot hide a crash. This keeps
PID-reuse fencing and bounded crash detection while removing filesystem and
iostream work from most messages.

### Experiment 2  remove redundant message-path heartbeat timestamps

Each successful Send and Receive called `steady_clock::now` and wrote the
heartbeat cache line even though a dedicated heartbeat thread already refreshes
the lease. The intended change is to let that thread own the heartbeat timestamp
and retain per-message `operation_sequence` updates for progress detection.

Both changes must pass the full fault matrix and sanitizer matrix before their
measurements count.
