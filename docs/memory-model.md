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
| endpoint.pid | release | acquire | PID publication follows endpoint metadata initialization. |
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

## Buffer and corruption semantics

The consumer does not advance tail when the destination is too small or a slot
length exceeds the configured maximum. This preserves evidence and avoids
silent loss. A recovery operation may explicitly abandon a corrupt generation;
normal Receive never guesses.
