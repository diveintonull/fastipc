# FastIPC upstream analysis

## Scope and evidence

FastIPC starts from kyr0/libsharedmemory at commit
9e24caaefb28826e99a33be2dd1350725558dd80. The upstream is a C++20,
header-only shared-memory library with POSIX, Windows, and macOS paths. This
audit describes the imported source before the FastIPC redesign. Exact
provenance and the tree hash are recorded in the workspace UPSTREAMS.md.

A clean Release/Ninja baseline of the pinned upstream completed its one CTest
target. That proves only that the baseline builds and passes its own suite; it
is not evidence for the new FastIPC requirements.

## Original architecture

One public header combines platform mapping, stream-like last-value storage,
and a queue. lsm::Memory owns a named mapping. Higher-level streams overlay
flags, revision, acknowledgement, payload length, writer lock, and data on
those bytes. lsm::SharedMemoryQueue overlays queue metadata and fixed-size
slots on another mapping.

There is no transport or protocol interface. Callers construct concrete stream
or queue types with a shared-memory name, size, persistence flag, and role.

## Shared-memory layout

The upstream queue implements this host-native layout:

    offset  size  field
    0       4     writeIndex
    4       4     readIndex
    8       4     capacity
    12      4     atomic count
    16      4     maxMessageSize
    20      4     atomic producerLock
    24      4     atomic consumerLock
    28      ...   capacity fixed-size slots

    slot := uint32 length + maxMessageSize bytes

The writer computes the mapping size from constructor arguments, unlinks an
existing POSIX object, creates a new object, and initializes the fields. A
reader maps a size computed from its own arguments, then replaces its local
capacity and maximum-message values with values from the mapping.

The layout has no magic, byte order, ABI-width marker, total-size field,
version, initialization state, generation, endpoint identity, heartbeat, or
checksum. The reader does not validate capacity, maximum message size, slot
length, or whether the object is large enough for trusted metadata. An old or
malformed segment can therefore be interpreted as a live queue.

## Queue structure and data flow

Enqueue acquires the producer spin lock, tests count, copies length and payload
into the current slot, advances writeIndex, release-increments count, and
releases the lock. Dequeue similarly acquires the consumer lock, tests count,
copies into a std::string, advances readIndex, release-decrements count, and
releases the lock. Peek takes the consumer lock without advancing.

The design admits multiple producers or consumers by serializing each side,
but attaches no owner identity to either lock. A process exiting while holding
a lock leaves future peers spinning indefinitely.

## Synchronization and C++ object model

The queue placement-constructs std::atomic<uint32_t> inside mapped bytes.
Other processes recover references with reinterpret_cast. Side locks use
acquire CAS, relaxed failure ordering, and release stores. Count uses acquire
loads and release fetch-add/subtract. Indices and slots are ordinary bytes
ordered indirectly by count and the side-specific locks.

The intent is understandable, but upstream does not document cross-process C++
object lifetime, audit lock-free requirements, or prove each happens-before
edge. FastIPC will make SPSC head and tail the publication variables, isolate
them by cache line, and document every ordering in docs/memory-model.md.

## Ownership and lifecycle

Memory owns a file descriptor and mapping. On POSIX the creator unconditionally
calls shm_unlink, creates mode 0777, calls fchmod before checking shm_open, then
truncates and maps. Destruction unmaps/closes and optionally unlinks according
to the persistence flag.

There is no atomic creator election or initialization handshake. Two creators
can unlink and replace the name while existing peers retain an old object.
Name ownership, mapping ownership, endpoint role, and generation are not
separate concepts, so restart cannot be recognized reliably.

## Blocking model and backpressure

Queue operations do not block: enqueue returns false when full and dequeue
returns false when empty. Callers must wait themselves. Side locks busy-spin
with yield. There is no futex, epoch, absolute deadline, timeout status,
cancellation, or lost-wakeup protocol.

Memory is bounded, which is useful, but overflow has only implicit immediate
failure. There are no Block, Timeout, or Drop policy objects and no counters
that explain nondelivery.

## Failure handling

Mapping calls return a small Error enum and higher layers mostly throw
runtime_error. Upstream does not detect or repair:

- incompatible or truncated layouts;
- corrupt slot lengths;
- duplicate producer or consumer ownership;
- process death while holding a lock;
- stale persistent segments;
- peer restart or PID reuse;
- stalled-but-alive peers;
- close while another operation waits.

## API and performance characteristics

The API is compact, but couples mapping, role, queue policy, payload, and
lifecycle. Data is copied into and out of fixed-size slots. Both peers update a
shared count, and all 28 metadata bytes share a cache line, causing avoidable
coherence traffic. Yield-based locks spend CPU and scheduler resources under
contention. Upstream records no latency percentiles, CPU, context switches, or
resident memory, so this audit makes no performance claim.

## Modification boundary

### Keep

- MIT license, notices, truthful upstream history, and attribution.
- Small CMake/CTest scaffold and the Linux named-mapping RAII concept.
- Fixed-capacity storage as a bounded-memory invariant.
- Baseline tests only as compatibility evidence during migration.

### Rewrite

- POSIX creation/opening, permissions, size checks, initialization election,
  mapping ownership, and cleanup.
- Byte layout as an explicitly versioned and validated Linux protocol.
- Queue as cache-line-isolated SPSC head/tail publication with audited
  acquire/release operations.
- Errors as status/result values for timeout, peer death, layout mismatch, role
  conflict, corruption, and shutdown.
- Backpressure as explicit Block, Timeout, and Drop policies with metrics.

### Remove

- Windows/macOS implementations from the focused Linux core.
- Legacy last-value streams and FFI surfaces that bypass the new lifecycle.
- Process-shared spin locks and the shared count hot spot.
- Implicit 0777 permissions and unlink-before-create behavior.

Removal does not erase attribution: imported commits and licenses remain even
when inherited files are later deleted.

### Add

- Magic, version, header/segment sizes, feature flags, generation, endpoint
  metadata, PID/start identity, heartbeat, initialization state, and epochs.
- Futex waits with monotonic absolute deadlines and recheck-before-sleep to
  close the lost-wakeup window.
- Duplicate-role rejection, stale detection, owner-death reporting, restart,
  and generation-aware reconnection.
- A Transport interface with shared-memory and Unix-domain-socket adapters;
  pipe remains a benchmark-only baseline.
- Unit, multiprocess, malformed-layout, fault-injection, and sanitizer tests.
- Reproducible 64 B through 1 MiB benchmarks recording throughput, P50/P95/P99,
  CPU, context switches, and memory, followed by measured profiling experiments.
