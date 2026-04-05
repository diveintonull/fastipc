# FastIPC fault matrix

## Contract

Every required fault is exposed as an independent CTest. Tests inject faults
outside the public adapter only when corruption itself is the subject; all
observations and recovery assertions use SharedMemoryTransport's public API.

| Required scenario | Injection | Expected observable result | CTest |
| --- | --- | --- | --- |
| Peer Missing | Open a consumer before the POSIX SHM object exists. | PeerUnavailable, preserving ENOENT as native detail. | fastipc.fault.peer_missing |
| Producer Crash | Create producer in a child, SIGKILL it, then block the consumer. | PeerDead within the bounded liveness probe; replacement producer advances generation and restores flow. | fastipc.fault.producer_crash |
| Consumer Crash | Create consumer in a child, SIGKILL it, fill the ring, then send again. | PeerDead; replacement consumer claims the role, drains committed messages, and restores flow. | fastipc.fault.consumer_crash |
| Restart | Gracefully close a persistent producer and reclaim the same segment. | Generation increases and the existing consumer adopts it. | fastipc.fault.restart |
| Slow Consumer | Fill a capacity-two ring and delay one receive. | Blocking producer sleeps, wakes on space_epoch, and completes without unbounded allocation. | fastipc.fault.slow_consumer |
| Timeout | Receive from an empty live channel with one fixed deadline. | Timeout without deadline extension. | fastipc.fault.timeout |
| Malformed Header | Change the mapped magic byte after closing a persistent producer. | LayoutMismatch from the public open operation. | fastipc.fault.malformed_header |
| Version Mismatch | Replace the mapped major-version field with an unsupported value. | LayoutMismatch from exact 1.1 layout validation. | fastipc.fault.version_mismatch |
| Stale Shared Memory | SIGSTOP a producer beyond its heartbeat lease and reclaim its role. | New generation succeeds; resumed old producer is fenced with StaleGeneration. | fastipc.fault.stale_shared_memory |
| Queue Full | Fill the ring, then invoke Drop and finite Timeout policies. | Dropped and Timeout, with both counters incremented once. | fastipc.fault.queue_full |
| Queue Empty | Block a consumer, publish later, and wake through data_epoch. | Receive completes with the exact payload and no lost wakeup. | fastipc.fault.queue_empty |
| Rapid Restart | Reclaim the producer role 16 times while one consumer stays mapped. | Generation is strictly increasing and every iteration restores message flow. | fastipc.fault.rapid_restart |

Additional aggregate coverage includes duplicate producer, duplicate consumer,
idle-heartbeat false-takeover prevention, stale-consumer role-token fencing,
graceful-close wakeup, message exchange, and a 2,000-message epoch stress run.

## Reproduce

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -L fault --output-on-failure
```

The fault label contains exactly the 12 scenarios required by the project
specification. The unlabelled fastipc.transport test runs the broader aggregate
suite in one process.
