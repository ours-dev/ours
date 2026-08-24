# Open: growing the dynamic area must not serialise allocation

Status: **recorded, not fixed.** Raised 2026-08-20 while reviewing
`kernel/mm/cpu-local.cpp`.

## The problem

`CpuLocal::Dyn::allocate()` currently takes `m_lock` and holds it across
`grow()`. `grow()` calls `Mapper::alloc_chunk()`, which does `alloc_frames()`
and — under the paged layout — `ArchVmAspace::map()` once **per unit**, i.e.
`MAX_CPU` times, and may build page tables on the way.

`sync::SpinLock` defaults to `Option::DisablePreempt`, so that whole run
happens with preemption off. Holding a spin lock across it is wrong on its own,
and it also makes every other CPU queue behind a slow operation none of them
needed.

## The shape it should have

Only *adding a chunk* needs mutual exclusion. Allocation itself should not.
A CPU that loses the race must not fail, and must not queue on the lock — it
should wait for the grower to publish, then go straight back to the (lock-free)
allocation path. Semantically a condition variable, with exactly one writer.

```
allocate():
  loop (bounded):
      off = try_allocate()                    // lock-free fast path
      if (off != kNowhere) return off

      seen = generation.load(acquire)
      if (grow_gate.try_acquire()) {          // I am the grower
          ok = grow()
          generation.fetch_add(1, release)    // publish -- even on failure,
          grow_gate.release()                 // or waiters spin forever
          if (!ok) return kNowhere
      } else {
          while (generation.load(acquire) == seen) { spin_hint(); }
          // resume at try_allocate(); never touch the gate
      }
```

Two details that are easy to get wrong: the generation must be bumped even when
`grow()` fails, or waiters never wake; and the loop must be bounded, so a
pathological interleaving terminates.

## What blocks it

`try_allocate()` is not lock-free today. `allocate_locked()` scans the bitmap
for a free run and then sets the bits — a read-modify-write spanning several
words. Until that changes, a waiter that wakes up still has to take a lock,
which is exactly what the design is trying to avoid.

Options, not yet decided:

- **A. Bump pointer + `fetch_add`, no reuse.** Trivially lock-free. Matches the
  earlier implementation under `.legacy`, whose `do_free` was
  `panic("Not support")`. But `MemCache::on_mem_cache_free` does exist, so
  caches are destroyed at least sometimes.
- **B. Bump pointer + lock-free free stack (Treiber) per size class.** Both
  paths CAS-only, no bitmap at all, and far cheaper than scanning. No
  coalescing, but per-CPU objects are few distinct sizes and long-lived.
- **C. Word-level CAS on the bitmap.** Keeps the bitmap's density and
  coalescing. A run crossing a word boundary cannot be claimed atomically, so
  it needs a rule such as "<= 64 cells lock-free, larger ones serialised".

Leaning B. Open question that decides it: how much does freeing per-CPU
allocations actually matter — is `on_mem_cache_free` a real path or effectively
never taken?

## Related

The lock also guards `owns()`, which `holds()` calls on every `resolve()` in
debug builds. If the chunk list becomes append-only and lock-free, that read
should become lock-free too rather than taking the allocator's lock.
