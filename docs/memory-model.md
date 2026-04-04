# FastIPC memory model

## Scope

This document audits the Linux SPSC shared-memory channel introduced after the
libsharedmemory baseline. The implementation is in
src/shared_memory_transport.cpp and the byte layout is in
src/shared_memory_layout.hpp. The public seam is Transport.

The contract targets Linux processes built for the same ABI. The implementation
uses GCC/Clang atomic builtins on naturally aligned 32-bit and 64-bit integers
inside MAP_SHARED memory. Compile-time checks require both widths to be always
lock-free. FastIPC does not claim that the ISO C++ abstract machine alone makes
process-shared atomics portable to every platform.

## Slot ownership

Head and tail are monotonically increasing 64-bit cursors.

- The producer owns head and is the only writer of a free slot.
- The consumer owns tail and is the only reader of a published slot.
- The producer writes SlotHeader and payload, then publishes head.
- The consumer observes head, copies the slot, then publishes tail.
- A slot can be reused only after the producer observes the released tail.

Head and tail occupy separate cache lines. Endpoint metadata, queue geometry,
epochs, and counters are cache-line isolated. There is no shared count modified
by both peers.

## Atomic-order audit

| Field / operation | Writer order | Reader order | Reason |
| --- | --- | --- | --- |
| header.init_state | release | acquire | Static layout fields are visible before validation. |
| header.generation | release store | acquire load | A replacement producer publishes a new session identity before endpoint ownership and messages. |
| endpoint.pid | release | acquire | PID publication follows endpoint metadata initialization. |
| endpoint.process_start_ticks | initialized before PID release | read after PID acquire | PID plus Linux start ticks distinguishes the original process from a recycled PID. |
| endpoint.role_token | initialized before PID release | acquire at operation entry and heartbeat update | A new claim fences an older endpoint even when channel generation is unchanged. |
| producer_cursor.head | relaxed load, release store | acquire load | Release publishes slot length, sequence, and payload; acquire makes them visible. Only the producer writes head. |
| consumer_cursor.tail | relaxed load, release store | acquire load | Release publishes completion of the slot read; producer acquire prevents early reuse. Only the consumer writes tail. |
| data_epoch | release fetch-add | acquire load | Orders the state transition before wakeup and provides a stable futex expected value. |
| space_epoch | release fetch-add | acquire load | Orders tail publication before producer wakeup. |
| heartbeat | release store | acquire load | Health observers see heartbeat after preceding progress. |
| operation sequence and counters | relaxed fetch-add | relaxed load | Diagnostics publish neither payload nor ownership. |

No sequentially consistent operation is used. A global total order would add
coherence constraints without strengthening the SPSC proof.

## Why the hot path has no CAS

The selected queue is SPSC. Head has one writer and tail has one writer, so CAS
would solve contention that cannot exist under the role invariant. Duplicate
role prevention belongs to setup and is serialized with a kernel file lock. If
MPSC is added, it needs a different reservation protocol and proof; it must not
be created by sprinkling CAS into this layout.

A failed compare-exchange would use relaxed ordering because failure publishes
nothing. A successful ownership CAS would need acquire-release ordering. Those
rules are recorded for an optional MPSC design, but this hot path deliberately
does not execute CAS.

## Futex epoch protocol

Waiting never treats a wake as the condition. The queue predicate remains head
versus tail.

Consumer empty path:

    acquire-load head and compare with local relaxed tail
    acquire-load data_epoch into expected
    recheck head versus tail
    futex WAIT_BITSET(data_epoch, expected, absolute monotonic deadline)

Producer full is symmetric with space_epoch. The recheck closes the lost-wakeup
window. If the peer changes state after the epoch snapshot but before syscall,
the futex value differs and the kernel returns EAGAIN. Spurious wakes and EINTR
return to the predicate loop.

After release-publishing head or tail, the peer release-increments its epoch and
calls FUTEX_WAKE. Epoch wrap is safe because every waiter also checks the queue
predicate; an epoch is a sleep-generation hint, not queue state.

## Absolute deadlines

Deadline stores one steady-clock target. Before each kernel wait, FastIPC
converts remaining duration to one CLOCK_MONOTONIC absolute timespec and uses
FUTEX_WAIT_BITSET. EINTR, EAGAIN, and spurious wakes cannot extend the deadline
because each conversion derives from the original target.

## Peer death and bounded probes

A blocked operation has two absolute deadlines: the caller's deadline and the
next liveness probe. FastIPC waits until the earlier one. A liveness-probe
timeout returns to the queue predicate instead of being reported as a caller
timeout. The next loop checks the peer's released PID, Linux process-start
ticks, and heartbeat lease. A missing PID is PeerUnavailable; a vanished or recycled
process identity, or an expired heartbeat, is PeerDead.

Every claimed endpoint owns one control-plane jthread. It refreshes heartbeat
at clamp(peer_timeout / 4, 1 ms, 250 ms), including while the data plane is
idle. The thread first verifies PID, start ticks, and role token, so it exits
after another endpoint claims the role. Close requests stop, wakes the local
condition variable, joins the heartbeat thread, and only then clears metadata
and unmaps.

SIGKILL cannot increment an epoch, so the bounded probe prevents an infinite
futex sleep after a crash. Graceful Close clears PID with release ordering,
increments the peer epoch, and wakes it immediately. Dead or expired role
takeover remains outside the SPSC hot path and is serialized by flock. Producer
takeover increments channel generation; all role takeovers publish a fresh
role token. Send and Receive verify their token before touching the ring, so a
resumed stale endpoint is rejected with StaleGeneration. This is cooperative
fencing at operation boundaries; strict recovery from a process frozen after its
final ownership check but before cursor publication would additionally require
generation-tagged slots or cursors.

## Buffer and corruption semantics

The consumer does not advance tail when the destination is too small or a slot
length exceeds the configured maximum. This preserves evidence and avoids
silent loss. A recovery operation may explicitly abandon a corrupt generation;
normal Receive never guesses.
