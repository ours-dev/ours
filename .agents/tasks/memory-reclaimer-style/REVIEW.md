# memory-reclaimer-style: review of how the two classes hold their state

**Date**: 2026-11-01

## Scope and method
Code shape of `MemoryReclaimer` and `MemoryWatchdog`: where the state lives, and what is still a bare global or a C calling convention. Files: `kernel/mm/memory_reclaimer.hpp`, `kernel/mm/memory_reclaimer.cpp`, `kernel/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp`.
Authorities: `modern-cpp-development` (one object owns the invariant, `Guard` rather than `acquire`/`release`, function-local statics in `SCREAMING_SNAKE`, `Thread::spawn` takes a plain function) and `lockdep::Guard::unlock()`.
Read-only. The earlier design review of who owes the distance is `.agents/tasks/memory-reclaimer-design/REVIEW.md`. This one does not repeat it.

## What is done well
- `memory_reclaimer.cpp:15-17` and `memory_watchdog.cpp:76-78`. The one instance is a function-local static, `RECLAIMER` and `WATCHDOG`. There is no mutable variable at namespace scope. The event, the levels, the pressure and the locks are members.
- `memory_reclaimer.cpp:23-26` and `memory_watchdog.cpp:92-94`. `Thread::spawn` takes a plain function and a `void *`. The lambda only turns that pointer back into the object and calls `run()`. The thread body is a member.
- `memory_reclaimer.cpp:56` and `memory_watchdog.cpp:86`. The walks and the level updates take the mutex through `Guard`.
- `memory_watchdog.hpp:159-172`. The levels and the declaration are `TA_GUARDED(m_lock)`. `pressure()` reads an atomic so a reader does not take the mutex.

## High severity
None.

## Medium severity
### M1. Hold takes the mutex by hand
`memory_watchdog.cpp:189-196`.
`Hold` calls `m_lock.acquire()` and `m_lock.release()`. Those two do not register with lockdep. `Guard` does, in its constructor, and `Guard::unlock()` drops the registration before the destructor runs. `semaphore.cpp` and `event.cpp` use that when the signal has to happen after the lock is down.
`Hold` needs the same order: the watchdog must be able to take `m_lock` in `look()` before it sees the signal. A member `Guard` constructed in `Hold`'s constructor, then `unlock()` and `signal()` in the destructor, keeps that order and puts the hold on lockdep's list. A test that nests `Hold` with another lock then reports the order. Today it does not.

### M2. look() finds the reclaimer through the singleton
`memory_watchdog.cpp:142` and `:149`.
`MemoryReclaimer::get()` is the one access the class offers, and that part is the usual shape. `look()` calling it means the watchdog's dependency is a process-wide name, resolved in the middle of a look. The reclaimer is not a member, so a reader of `MemoryWatchdog` cannot see what `look()` mutates without opening `get()`.
Pass the reclaimer into `start()`, and keep it as a `MemoryReclaimer &` member. `get()` stays the way boot obtains the one instance. `look()` then calls the member. `Pause` has the same reach: it calls `get()` itself (`memory_reclaimer.hpp:61`), while `Hold` takes the watchdog it holds. `Pause` should take a `MemoryReclaimer &` the way `Hold` takes a `MemoryWatchdog &`.

### M3. MemoryPressureLevels does not own its invariant
`memory_watchdog.hpp:37-77` and `memory_watchdog.cpp:62-73`.
The type is an aggregate with public fields. `is_valid()` is a method a caller may skip. `delay_level()`, `bounds_of()` and `pressure_at()` then subtract and compare as if the levels nested. An invalid value underflows `out_of_memory - debounce`, which is unsigned. `start()` and `set_levels()` do check. Any other caller of the methods, including a test that builds a levels value and asks `bounds_of` directly, does not.
A factory that returns only a valid value, with the fields private after that, puts the check in the type. Designated initialisers stop working, because a user-declared constructor ends the aggregate. The call sites that fill the five fields move to that factory. `from_boot_options()` already is one.

## Minor and style
- `memory_watchdog.hpp:163` and `:172`. `m_skip_hysteresis` and `m_declared_once` are two bools under `m_lock`. A flags word is the convention for facts about the object's state guarded together.
- `memory_reclaimer.cpp:20` and `memory_watchdog.cpp:81`. `start()` does not remember that it spawned. A second call starts a second thread on the same members, and `AutoUnsignal` wakes only one of them. The thread is not a member. A member set under the lock, checked at the start of `start()`, makes "already started" part of the object.
- `memory_watchdog.cpp:19-30`. `to_string` returning `char const *` matches the log. Leave it.

## Teaching opportunities
### T1. The void * is the adapter, not the state
`Thread::spawn` and `GKTL_INIT_HOOK` both take a plain function. The hook in `memory_reclaimer.cpp:86` and the lambda in `start()` are that function. The state they touch is the object `get()` returns.
A namespace-scope `Event` or a namespace-scope `usize` would be the bare global. Nothing here is one. Replacing `get()` with a namespace-scope object would put the global back.
What is still global is the lookup: `look()` and `Pause` name `get()` instead of an object they were given. That is M2.

### T2. acquire and release are not a Guard
The invariant lockdep keeps is the set of locks the thread holds, and the order it took them. `Guard`'s constructor registers, `unlock()` unregisters and releases, the destructor does `unlock()` if the caller has not.
`Hold` takes and drops `m_lock` without registering. The worst interleaving a test can build is `Hold` around a call that takes some other mutex, while the watchdog later takes that mutex and then `m_lock`. Lockdep never saw `Hold`, so it never reports the cycle. `Guard::unlock()` before `signal()` is the same order `Hold` has now, with the registration added.

## Suggested follow-up order
1. `Hold` becomes a `Guard` and calls `unlock()` before `signal()`. M1.
2. `start(MemoryReclaimer &)` stores the reference, and `Pause` takes one too. M2.
3. A factory for `MemoryPressureLevels` that does not return an invalid value. M3.
4. Leave `get()`, the `void *` lambda, and the init hook as they are.
