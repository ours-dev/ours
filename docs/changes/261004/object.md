# object changes

## Title: A port: port_create, port_queue and port_wait (K26) (2026-10-04)

**Files**
- `system/public/ours/port.hpp` — new: `PacketType`, `PacketSignal`, `PortPacket` (Zircon's 48-byte `zx_port_packet_t`, pinned by static_assert), `PortCreateOptions`, `PORT_MAX_USER_PACKETS`
- `system/public/ours/types.hpp` — `ObjectType::Port = 6`, Zircon's value
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` — new: `PortDispatcher` and `QueuedPacket`
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/port.cpp` — new calls `port_create`, `port_queue`, `port_wait`
- `kernel/lib/syscall/include/syscall/handlers.hpp`, `system/public/ours/vdso.hpp`, `system/public/public_headers.gni`, both `BUILD.gn` files, `kernel/object/include/ours/object/types.hpp` — wiring
- `examples/shell/shell_tests.cpp` — `check_port()`
- `docs/design/260923/userspace-interface.md` — the revisions K26 owes it (section 7.4 of the filesystem design), as English notes

**Symptom**
None, a planned change: K26 of `docs/design/260924/userspace-filesystem.md`, section 7.4. Until now a thread could wait on one object at a time (`object_wait_one`). init's event loop (U10) has to wait on many at once, and a port is the queue those waits will deliver into once `object_wait_async` exists (K27). This step is the queue alone, testable with `port_queue`, so that a bug in the queue cannot hide behind one in the observers.

**Cause**
The part worth learning is how waiters sleep, and why it differs from Zircon.

The objects:
- `m_packets`, the queue, and `m_user_packets`, guarded by `m_port_lock`.
- `m_ready`, a manual-reset `sync::Event`. Once signalled it stays signalled, and every `wait()` returns at once, until `unsignal()`. That latch is what stops a signal that arrives before its waiter from being lost.
- A waiter: a thread in `port_wait`, which takes the first packet or sleeps on `m_ready`.

The rule: `m_ready` is signalled and unsignalled only under `m_port_lock`, so "signalled" and "the queue is not empty" are one fact. A waiter that finds the queue empty drops the lock and waits. A packet queued in that gap leaves the event signalled, so the wait returns at once and the packet is not missed.

What goes wrong without the rule, if the producer signals after dropping the lock as Zircon does:
1. Producer P locks, queues a packet, unlocks.
2. Consumer C locks, takes the packet. The queue is now empty, so C unsignals, then unlocks.
3. P now signals. The event is signalled over an empty queue.
4. Waiter W finds the queue empty and waits. The wait returns at once, W finds the queue empty again, and it loops: a spin that never sleeps.

Zircon can post after the lock (`QueueAndRemoveObserver()` calls `sema_.Post()` after dropping the guard, so that the woken thread does not block on the lock) because its waiters sleep on a semaphore. A stale post there costs one extra wake-up, which then sleeps again; it cannot latch. A manual-reset event can latch, so the order matters here.

The cost: a signal wakes every waiter, though only one can get the packet. Today one process has one thread, so a second waiter can only be another process with a duplicated handle. When a second thread waits on one port, `sync::Semaphore` replaces the event.

Two more choices from section 7.4:
- **The port has its own lock**, not the `Dispatcher` `m_lock`. From K27 an observer queues a packet while holding the observed object's `m_lock`, so "object lock, then port lock" needs two locks lockdep can tell apart.
- **Nothing queues after the last handle closes.** `on_zero_handles()` sets `m_zero_handles`, empties the queue, unsignals, and frees the packets outside the lock. `queue_user()` checks the flag first and answers BadHandle. A syscall still holding an `Arc` from its handle lookup can reach the port after its last handle closed. Once observers hold the port (K27), a packet queued then would keep the port alive in a cycle with no handle to break it.

**Change**
The waiter's loop (`PortDispatcher::dequeue()`):

```cpp
for (;;) {
    ustl::Box<QueuedPacket> taken;  // freed after the guard below
    {
        Guard guard{m_port_lock};
        if (auto front = m_packets.front()) {
            ...erase it, m_user_packets -= 1...
            if (m_packets.empty()) {
                m_ready.unsignal();
            }
            *out = packet.packet;
            taken = ustl::Box<QueuedPacket>::from_raw(&packet);
        }
    }
    if (taken) return Status::Ok;
    if (deadline == 0) return Status::TimeOut;
    if (auto const status = m_ready.wait(time::Deadline::infinite()); status != Status::Ok) {
        return status;
    }
}
```

The producer (`queue_user()`) allocates before the lock and pushes under it, then signals still under the lock. It keeps the caller's key, status and payload, and forces only `type = User`, as Zircon's `QueueUser()` does.

The calls follow this tree's error order (interface design 4.5), the handle first (BadHandle, BadType, AccessDenied):
- `port_queue` then reads the packet (InvArgs if it cannot).
- `port_wait` then checks the deadline (NotSupported for anything but 0 or `INSTANT_INFINITE`, as `object_wait_one` until P1-E), then takes a packet or answers TimeOut to a poll, and only then writes the packet out. A packet it cannot write out is lost, as in Zircon's `sys_port_wait()`: putting it back would let one bad pointer jam the port for every waiter. The rights are Zircon's `ZX_DEFAULT_PORT_RIGHTS`: Basic without Wait, plus Read and Write. A port asserts no signals, so it cannot be waited on, and so cannot be watched through another port.

**Reference**
- Zircon `PortDispatcher::Create()`, `QueueUser()`, `QueuePacketLocked()`, `Dequeue()` and `on_zero_handles()` in `zircon/kernel/object/port_dispatcher.cc`.
- Zircon `sys_port_create()`, `sys_port_queue()` and `sys_port_wait()` in `zircon/kernel/lib/syscalls/port.cc`.
- `zx_port_packet_t` and `zx_packet_signal_t` in `zircon/system/public/zircon/syscalls/port.h`.

**Verification**
- `boot_check.py`: dbg PASS (21 passed, one more than before), rel PASS (19 passed).
- Each shell check on both images:
  - an unknown create option is InvArgs;
  - a SignalOne packet with status InvArgs is read back with the same key, status and payload, and type User;
  - a poll on an empty port is TimeOut;
  - deadline 5 is NotSupported;
  - 256 packets fit and the 257th is ShouldWait;
  - `port_wait` on a channel is BadType;
  - `object_wait_one` on a port is AccessDenied.
- Closing the port with 256 packets queued: "object: port (koid N) destroyed" follows the shell's marker line and comes before `close(port) = Ok`, on both images.
- clang-tidy over `port_dispatcher.cpp` and `src/port.cpp`: nothing reported.
- A review workflow (two reviewers, each finding put to a refuter) left two standing findings, both wording: the Zircon limit described in `port.hpp`, and the error order above. Both are fixed. A refuted one was taken anyway: the read-back now starts from a packet filled with 0xFF, so a field the kernel failed to write cannot pass.
- Not covered: a `port_wait` interrupted by a suspend returns InternalIrqRetry to the program, because the `blocking` attribute does not yet make the vDSO retry. Nothing can suspend a user thread yet, and the gap is already recorded with `task_suspend` (`docs/changes/261003/kernel-task.md`).

**Deliberately not done**
- `object_wait_async`, the observers and their limit (`PORT_MAX_OBSERVERS`, `WaitAsyncOptions`, `QueuedPacket`'s owner pointer): K27 and K28.
- `port_cancel`: M1's Loop never takes back a wait.
- Deadlines other than 0 and infinite: P1-E, with a user-readable clock.
- A test with two waiters. Within one process there is no second thread yet. The rule above is argued, not measured.
- Two items the plan listed are gone already. `Event::unsignal()` was fixed to hold its lock with interrupts off by `5a205886` (2026-09-25). The `ou` library was removed by `5692032a`, so there is no forwarder to add.

**How it was found**
None.

**The user's prediction**
None. The design and its lock order were decided on 2026-09-25 (D4 of the filesystem design); the user asked to start K26 directly.

**Three questions you should be able to answer**
- Why must `m_ready` be signalled and unsignalled under the port lock here, when Zircon posts its semaphore after dropping its lock?
- What does a waiter woken to an empty queue do, and why does it not spin?
- Why does `queue_user()` check `m_zero_handles` before anything else, and what would leak without it once observers exist?

## Title: object_wait_async through a one-shot PortObserver (K27) (2026-10-04)

**Files**
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` — `PortObserver`; `QueuedPacket::owner`; the port's `queue_observer_packet()` and `maybe_reap()`; `dequeue()` and `on_zero_handles()` free an observer-owned packet through its observer
- `kernel/object/include/ours/object/handle_table.hpp`, `kernel/object/handle_table.cpp` — `attach_port_observer()`: both handles in one hold of the table lock
- `kernel/object/handle.cpp` — `~Handle` cancels the waits made through it
- `kernel/object/include/ours/object/dispatcher.hpp`, `kernel/object/dispatcher.cpp` — `~Dispatcher` asserts that no observer is still attached
- `system/public/ours/port.hpp` — `WaitAsyncOptions`
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/object.cpp` — `object_wait_async`
- `examples/shell/shell_tests.cpp` — `check_wait_async()`

**Symptom**
None, a planned change: K27 of `docs/design/260924/userspace-filesystem.md`, section 7.4. After K26 a port could only be fed by `port_queue`. A thread still could not wait on two objects at once, and init's event loop (U10) needs exactly that.

**Cause**
Who frees a PortObserver is the part worth learning.

The objects:
- **The observer.** It holds its packet (key and trigger written at construction), an `Arc` to the port, and `m_object`, a bare pointer to the watched object. `m_object` is non-null while the observer is registered. It is read and written under the port lock.
- **The watched object's observer list**, under that object's `m_lock`. A signal change, or a level check at attach, unlinks each observer it satisfies and calls its `on_match()`. A handle leaving its table unlinks the observers made through it and calls `on_cancel()`.
- **The port's queue**, under the port lock.

The rule: whoever sets `m_object` to null frees the observer. The events, for each way a wait can end:
1. **It matches.** Under the object's lock, `on_match()` calls the port's `queue_observer_packet()`, which takes the port lock, nulls `m_object` and queues the observer's own packet. The queue now owns the observer: `port_wait` frees it when it takes the packet, and `on_zero_handles()` when it empties the queue.
2. **Its handle leaves the table.** Under the object's lock, `on_cancel()` calls `maybe_reap()`, which takes the port lock, finds `m_object` set, nulls it and hands the observer back to be freed. Nothing is queued.
3. **It matches after the port lost its last handle.** `queue_observer_packet()` sees `m_zero_handles`, nulls `m_object` and hands the observer back instead of queuing. Queuing would keep the port alive through the observer's `Arc`, with no handle left to break that cycle.

What keeps two of these from freeing the same observer is not `m_object` but the watched object's lock. `on_match()` and `on_cancel()` are each called under that lock by the code that has just unlinked the observer (an immediate match never links it), so whichever comes first is the only one that finds it. `m_object` arbitrates a race that lock does not cover: a closing port tearing down its armed observers under the port's lock, which is K28. Until then it is always set when a callback looks.

In each case the observer is freed by its caller after the port lock is dropped. Freeing it drops an `Arc` to the port, perhaps the last one, and `~PortDispatcher` runs right there, under the watched object's lock. That is why `~PortDispatcher` takes no lock and only asserts. The shell shows it: a port closed with a wait still armed is destroyed when the wait fires, during the `channel_write` that fires it.

Two more things the bare pointer forced:
- **`~Handle` cancels too.** An observer is attached through a handle and must not outlive it. Every handle leaves a table before it dies, and the table cancels then, so today this finds nothing. It is the backstop for any future path that destroys a handle without a table: an observer left linked there would never be freed, and would hold its port. Zircon's `HandleTableArena::Delete()` cancels the same way. `~Dispatcher` now asserts that its list is empty.
- **Both handles in one table lock.** `attach_port_observer()` resolves the port, then the object, checks the options, and attaches, all in one hold of the table lock, as Zircon's `sys_object_wait_async()` does. A close racing the call then behaves as if it came wholly before or wholly after it, and the object's handle cannot leave the table between being found and being watched.

**Change**
The observer's two callbacks, and the port side of the first:

```cpp
auto PortObserver::on_match(Signals const signals) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    // Destroyed before |port|: declared after it.
    auto release = port->queue_observer_packet(this, signals);
}

auto PortObserver::on_cancel(Signals const) -> void {
    ustl::Arc<PortDispatcher> port = m_port;
    auto release = port->maybe_reap(this);
}

auto PortDispatcher::queue_observer_packet(PortObserver *observer, Signals observed)
    -> ustl::Box<PortObserver> {
    Guard guard{m_port_lock};
    observer->m_object = nullptr;
    if (m_zero_handles) {
        return ustl::Box<PortObserver>::from_raw(observer);
    }
    ...observed, count = 1...
    m_packets.push_back(observer->m_packet);
    m_ready.signal();
    return {};
}
```

The local `Arc` copy has two jobs. `ustl::Rc` passes const on, so a method through the `Arc<PortDispatcher> const` member cannot reach the port's non-const members. And the copy keeps the port alive while its method finishes, after the observer it was called for has been freed. Zircon's `OnMatch()` takes `port_ref` for the second reason.

The error order of `object_wait_async` (interface design 4.5):
- the port: BadHandle, then BadType, then AccessDenied without Write;
- the object: BadHandle, then AccessDenied without Wait;
- then InvArgs for the options, then NoMemory.

Zircon checks its options before taking the table lock. Here they come after both handles, so that a bad handle outranks a bad option. Nothing is attached before every check passes, because a wait attached and then taken back could already have queued its packet, and there is no `port_cancel` to withdraw it.

Two departures from the plan, both smaller:
- The attach goes through the existing `Dispatcher::attach_observer()`, not a new `attach_port_observer()` on `Dispatcher`. The plan's version registers the observer with the port before the level check, so that a closing port could find it. That registration is K28's tear-down; until then there is nothing to register.
- No `PORT_MAX_OBSERVERS` yet, which is also K28.

**Reference**
- Zircon `PortObserver::OnMatch()`, `OnCancel()`, `PortDispatcher::QueueAndRemoveObserver()`, `QueuePacketLocked()` and `MaybeReap()` in `zircon/kernel/object/port_dispatcher.cc`.
- Zircon `sys_object_wait_async()` in `zircon/kernel/lib/syscalls/object_wait.cc`.
- Zircon `HandleTableArena::Delete()` in `zircon/kernel/object/handle.cc`, and `Dispatcher::~Dispatcher()`.

**Verification**
- `boot_check.py`: dbg PASS (22 passed), rel PASS (20 passed).
- Each `check_wait_async()` line on both images:
  - a wait on an idle b queues nothing (TimeOut);
  - a write to a queues `{key 7, SignalOne, Ok, trigger Readable, observed Readable|Writable, count 1}`, and a second poll is TimeOut;
  - a wait on a b already readable queues at once;
  - an unknown option is InvArgs and leaves nothing queued;
  - sending b away through a second channel delivers the packet queued before it left, and nothing for its armed PeerClosed wait, although closing a then raises PeerClosed on the end (read back from the second channel, `wait_one` says so);
  - a wait on a port is AccessDenied, and a channel as the port argument is BadType.
- A port closed with a wait armed: "object: port (koid N) destroyed" appears after the shell's second marker, during the write that fires the wait, and not after the first (when the port's handle was closed), on both images.
- clang-tidy over the five changed sources: nothing new beyond warnings in lines this change did not touch.
- HEAD plus only these files, in a scratch copy: dbg PASS (22), rel PASS (20).
- The cancel check can fail: with the cancel removed from `HandleTable::unlink_locked()` in the scratch copy, rel prints "port_wait(the armed wait ended with b) = Ok (expected TimeOut)" and "wait_async checks FAILED".
- A review workflow (three reviewers, each finding put to a refuter) left two standing findings. One: the comment and this record credited `m_object` with keeping `on_match` and `on_cancel` apart, when it is the object lock; both are corrected above. Two: the cancel check could not fail, because closing b destroyed the end, so closing a never raised PeerClosed on it. The check now sends b through a second channel instead of closing it, which cancels the waits made through it but keeps the end alive. Taken from the reviewers' smaller notes: the spent-wait check makes Readable fall and rise again, the packet's timestamp and reserved fields are compared, and the docs of `queue_observer_packet()` and `Dispatcher::cancel()` now name the locks really held.

**Deliberately not done**
- K28: tearing armed observers down when the port closes, and bounding them (`PORT_MAX_OBSERVERS`, counting queued signal packets). Until then a closed port lives as long as its armed waits.
- `port_cancel`, timestamps and edge-triggered waits: P2, or never for M1's Loop.
- A stress test of a wait firing while its handle is closed from another process. The object lock serialises the two callbacks, so the outcome is one of them, never both; that is argued from the code, not measured.

**How it was found**
None.

**The user's prediction**
None. The design was decided on 2026-09-25 (D4 of the filesystem design), and the user asked to start.

**Three questions you should be able to answer**
- Three paths can end a PortObserver's wait. For each, who sets `m_object` to null, and who frees the observer?
- Why can `~PortDispatcher` run under the lock of a channel the port never touched, and what does that rule out in the destructor?
- Why does `attach_port_observer()` check the options only after both handles, and why must nothing be attached before every check passes?

## Title: A closing port tears its armed observers down, and PORT_MAX_OBSERVERS bounds them (K28) (2026-10-04)

**Files**
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` — `m_armed` and `m_live_observers`; `register_observer()`; the tear-down loop in `on_zero_handles()`; the `m_object` checks in `queue_observer_packet()` and `maybe_reap()`; two counters and `tear_down_counts()`. `PortObserver` moves ahead of `PortDispatcher` so that the port can hold a list of observers.
- `kernel/object/include/ours/object/dispatcher.hpp`, `kernel/object/dispatcher.cpp`, `kernel/object/include/ours/object/types.hpp` — `Dispatcher::attach_port_observer()`
- `kernel/object/handle_table.cpp` — attaches through it, and frees an observer the port refused
- `system/public/ours/port.hpp` — `PORT_MAX_OBSERVERS` (1024)
- `system/public/ours/syscalls.gni`, `kernel/object/include/ours/object/handle_table.hpp` — `object_wait_async`'s answers gain NoResource, and its waits end when the port loses its last handle
- `examples/shell/shell_tests.cpp` — the closed-port check expects the tear-down; `check_port_observer_limit()`
- `kernel/commands/port_test.cpp`, `kernel/commands/BUILD.gn` — new: `port.test [ROUNDS]`

**Symptom**
Before this step, a port closed with waits still armed was not destroyed. Each armed observer holds a reference to its port, so the port lived until every one of those waits fired or was cancelled. For a wait on something that never happens, that was for ever. K27's shell check showed it: the port was destroyed only during the write that fired its last wait. A program could also arm waits without limit.

**Cause**
The tear-down is a race with the observer's own callbacks, and it is the one race in the port that a single lock does not settle.

The objects:
- `m_armed`, on the port: the observers registered and not yet matched. It is guarded by the port lock. An observer is put there by `register_observer()` and taken off by whichever path ends it.
- `m_object`, on each observer: non-null while it is registered. It is read and written under the port lock. Whoever sets it to null frees the observer.
- The watched object's observer list, under the object's lock, from which the object unlinks an observer before calling `on_match()` or `on_cancel()`.

The events, when the port closes on one CPU while the watched object fires on another:
1. The port (`on_zero_handles()`), under the port lock: it takes the observer off `m_armed`, takes a reference to the object, sets `m_object` to null, and drops the port lock. The observer is now the tear-down's.
2. The object, on the other CPU, under its own lock: it unlinks the observer and calls `on_match()`, which waits for the port lock and then finds `m_object` null. It queues nothing and frees nothing.
3. The port, with no port lock held: `detach_observer()` waits for the object's lock, so for step 2 to finish, and finds the observer already unlinked. Then the tear-down frees it.

In the other order, the callback takes the port lock first. It finds `m_object` set, nulls it, and takes the observer off `m_armed`, so the tear-down never sees it.

Why each piece is needed:
- **The reference to the object** (`Arc<Dispatcher>(m_object)`) keeps the object alive across step 3, after the port lock is gone. It is safe to take because `m_object` is still set: the handle the wait was attached through has not finished dying, and that handle holds the object.
- **The port lock is dropped before `detach_observer()`.** The only lock order is object, then port. A port that held its lock while taking an object's would deadlock against step 2.
- **Registration happens under the object's lock, before the level check** (`Dispatcher::attach_port_observer()`, Zircon's `MakePortObserver()`). Registering and linking are then one step as far as the tear-down can see. The tear-down's `detach_observer()` needs the same lock, so it finds the observer either linked or already handed to `on_match()`. An observer linked after the tear-down had passed would never be freed.

The limit counts armed observers and observers whose packets are still queued, which Zircon's does not (design section 10). Otherwise a program could arm a wait on a signal that is already asserted, have it queue at once, and arm again without end.

**Change**
The tear-down loop in `on_zero_handles()`, after the queue has been emptied:

```cpp
for (;;) {
    PortObserver *observer = nullptr;
    ustl::Arc<Dispatcher> object;
    {
        Guard guard{m_port_lock};
        auto front = m_armed.front();
        if (!front) break;
        observer = &*front;
        auto _ = m_armed.erase(m_armed.iterator_to(*observer));
        m_live_observers -= 1;
        object = ustl::Arc<Dispatcher>(observer->m_object);
        observer->m_object = nullptr;
    }
    auto _ = object->detach_observer(observer, &ignored);
    auto torn_down = ustl::Box<PortObserver>::from_raw(observer);
}
```

The callbacks' side, in `queue_observer_packet()` and `maybe_reap()`:

```cpp
if (observer->m_object == nullptr) {
    return {};  // the tear-down has it
}
```

`m_live_observers` goes up once, at registration, and down once, at whichever of these ends the observer:
- `dequeue()` taking its packet;
- `on_zero_handles()` clearing its packet;
- the zero-handles drop in `queue_observer_packet()`;
- `maybe_reap()`;
- the tear-down.

**Reference**
- Zircon `PortDispatcher::on_zero_handles()`, `MakeObserver()` and `MaybeReapLocked()` in `zircon/kernel/object/port_dispatcher.cc`. Its tear-down runs `RemoveObserver()` inside `CallUnlocked()` for the same lock-order reason.
- Zircon `Dispatcher::MakePortObserver()` in `zircon/kernel/object/dispatcher.cc`.

**Verification**
- `boot_check.py`: dbg PASS, rel PASS (21 passed on rel, one more than K27).
- On both images, "object: port (koid N) destroyed" now comes between the shell's "closing a port with 3 waits armed" and the line after the close, and the later write to the watched channel produces nothing. On K27's code the same check showed the port destroyed only during that write.
- `check_port_observer_limit()`, both images:
  - waits on an idle end are refused at exactly 1024 (NoResource), and closing that end lets a new wait in;
  - waits on a readable end each queue at once and are refused at 1024 as well; closing the end changes nothing, and reading one packet makes room for one wait.
- `port.test 300` on the dbg image, two CPUs. Each round arms 64 raced waits on one channel end and 8 stranded waits on the other, which only the tear-down can end. The port is closed on one CPU while the other, after a delay that differs from round to round, ends the raced waits. In even rounds it raises Readable. In odd rounds it closes the end as a handle close would: it cancels the waits, then drops what may be the end's last reference, so that the tear-down's own reference to the object is what keeps it alive.
  - Result: 5600 observers torn down (at least 300 × 8 required), 204 callbacks that met the tear-down and yielded (at least one required), 0 bad rounds, no assertion.
  - A round is bad if a weak reference finds the port alive after both threads are joined. In dbg, `~Dispatcher` also asserts that nothing is still linked on either end as it goes.
  - First, once, the command drives a handle table of its own past `PORT_MAX_OBSERVERS`: 1024 waits armed, then NoResource, then the port's handle closed. The port was destroyed, so the observers the table made for refused waits were freed.
- The two checks fail when they should. Each mutation was run in a scratch copy:
  - Without the tear-down loop, round 0 reports "the port outlived its last handle", and the refusal check fails too, since 1024 armed waits hold the port. Destroying the round's ends then stops the dbg kernel at `~Dispatcher`'s assertion.
  - With the refused observer not freed in `HandleTable::attach_port_observer()`, the command prints "a wait refused at the limit was not freed" and FAILED.
- `port.test` runs only on the dbg image. The rel image's console belongs to the user shell (`kernel.shell=false`), which takes the command for a program in `bin/`.
- clang-tidy over the four changed kernel sources: nothing reported.
- Built and booted as HEAD plus only these files, in a scratch copy: dbg PASS (24), rel PASS (21). The shared tree did not build at the time because of another session's work in progress (`thread_dispatcher.cpp`, exception channels).
- A review workflow (two reviewers, each finding put to a refuter) left five standing findings:
  - `object_wait_async`'s documented answers lacked NoResource and the port-close ending;
  - `port.test`'s weak check could not fail without the tear-down, because the ender ended every wait;
  - the cancel rounds never needed the tear-down's reference to the object;
  - nothing that can fail covered the refused observer's free;
  - the test's failure paths leaked, and a cleanup cancel made `~Dispatcher`'s assertion unreachable while a comment said it was a check.
  All five are fixed as described above: the stranded waits, the cancel rounds that drop the end, the refusal check, and frees on the failure paths. A refuted finding stays open by design: the shell's closed-port check is read from the boot log, as the K28 plan specifies.

**Deliberately not done**
- lockdep: no image links it (`docs/repairs/260917/kernel-task-sched.md`, item 6), so the lock order is argued, not checked.
- `port_cancel`, timestamps and edge-triggered waits.
- A Semaphore in place of the event (one wake per packet), which waits for a second thread waiting on one port.

**How it was found**
None.

**The user's prediction**
None. The user asked to go straight to the implementation.

**Three questions you should be able to answer**
- In the race between the port's tear-down and an `on_match()`, who frees the observer in each order, and why is it never freed twice?
- Why must the tear-down drop the port lock before `detach_observer()`, and what keeps the object alive while it does?
- Why does registration happen under the object's lock and before the level check?

## Title: Interrupt objects, virtual first: InterruptDispatcher, VirtualInterruptDispatcher and four system calls (2026-10-04)

**Files**
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — new: `InterruptDispatcher`, the base of the interrupt family, with Zircon's state machine (Idle, Waiting, Triggered, NeedAck, Destroyed), the spinlock L2, the auto-unsignal event a synchronous wait parks on, and the four pure virtuals on the hardware
- `kernel/object/include/ours/object/virtual_interrupt_dispatcher.hpp`, `kernel/object/virtual_interrupt_dispatcher.cpp` — new: `VirtualInterruptDispatcher`, whose four hardware operations are empty
- `kernel/include/ours/sync/event.hpp`, `kernel/kernel/sync/event.cpp` — `Event` takes `Options` (None or AutoUnsignal) in a constructor; the protected `Flags` nobody could set is gone
- `system/public/ours/interrupt.hpp` — new: `InterruptOptions` (Virtual = 0x10, Zircon's value) and `InterruptTriggerOptions`
- `system/public/ours/types.hpp` — `ObjectType::Interrupt = 9`, Zircon's `ZX_OBJ_TYPE_INTERRUPT`
- `system/public/ours/syscalls.gni`, `kernel/lib/syscall/src/interrupt.cpp` — new calls `interrupt_create` (Virtual only), `interrupt_wait`, `interrupt_trigger`, `interrupt_destroy`
- `kernel/lib/syscall/include/syscall/handlers.hpp`, `system/public/ours/vdso.hpp`, `system/public/public_headers.gni`, `kernel/object/BUILD.gn`, `kernel/lib/syscall/BUILD.gn`, `kernel/commands/BUILD.gn` — wiring
- `examples/shell/shell_tests.cpp` — `check_interrupt()`, everything one thread can check, on both images
- `kernel/commands/interrupt_test.cpp` — new: `interrupt.test`, the checks that need a second thread to park and wake
- `docs/repairs/261004/object.md` — new: the shell's missing parked-wait checks, and a handoff time worth measuring

**Symptom**
None, a planned change: step 2 of `.agents/tasks/microkernel-interrupts/PLAN.md`. A user-space driver needs an interrupt object to wait on before any line can reach it (DESIGN.md, D1 (b): virtual first, physical lines in step 11). Defect 13 of that design was latent until now: `sync::Event` had an auto-unsignal mode whose bit no one could set, so the branches that implement it in `event.cpp` had never run.

**Cause**
The part worth learning is why the synchronous wait needs an auto-unsignal event and also an explicit unsignal under the lock, which looks like doing the same thing twice.

The objects:
- `m_state`, where the interrupt is in its life, and `m_timestamp`, the time of the first interrupt not yet taken. Both guarded by `m_spinlock` (L2), a spin lock held with interrupts off, because from step 11 a physical line's handler takes it in IRQ context.
- `m_event`, an auto-unsignal `sync::Event`. A signal goes to exactly one parked waiter; with none parked, it is latched, and the next `wait()` consumes it and returns at once.
- The waiter: a thread in `interrupt_wait`, which calls `begin_wait()` under L2 (take a pending interrupt, or set Waiting) and then, outside L2, `park()` on the event.
- The trigger: `interrupt_trigger` today, the line's handler from step 11. Under L2 it records the time if none is pending, signals the event, and sets Triggered (or stays in NeedAck).

Why the event, and not "check the state, then sleep": the waiter must drop L2 before it sleeps, because sleeping under a spin lock with interrupts off is not possible. In the gap between dropping L2 and parking, a trigger can land. A latched signal is what carries that trigger across the gap: the trigger finds nobody parked, latches, and the waiter's park returns at once. Without the latch the trigger is lost and the waiter sleeps through it.

Why auto-unsignal, and not a manual-reset event: a manual-reset event stays signalled until someone unsignals it, and every wait returns at once while it does. After one trigger, every later wait would spin through `park()` without sleeping. Auto-unsignal consumes the signal with the wait it wakes. Zircon's `InterruptDispatcher::event_` is an `AutounsignalEvent` for this reason (`zircon/kernel/object/include/object/interrupt_dispatcher.h:118`).

Why also unsignal under the lock, though the event is auto-unsignal: a wait can take an interrupt without parking at all. The interleaving, with one thread T and a trigger:
1. A trigger arrives while T is not waiting. Nobody is parked, so the signal is latched, and the state becomes Triggered.
2. T calls `interrupt_wait`. `begin_wait()` sees Triggered, takes the time, sets NeedAck, and returns. T never called `m_event.wait()`, so the latched signal is still standing.
3. T calls `interrupt_wait` again, to ack and wait for the next one. `begin_wait()` sees NeedAck with nothing pending, sets Waiting, and T parks. The stale signal wakes it at once, for an interrupt it already took.
4. T loops into `begin_wait()`, finds Waiting (its own state), and answers BadState.

So every answer of `begin_wait()` that takes an interrupt calls `m_event.unsignal()` while it still holds L2, as Zircon's `BeginWaitForInterrupt()` returns `event_.Unsignal()` in its Triggered and NeedAck cases (`zircon/kernel/object/interrupt_dispatcher.cc:65`, `:89`). Under L2 matters: a trigger also signals under L2, so it cannot slip between the take and the unsignal and have its own signal erased.

The event itself: `Event` had `enum class Flags { None, AutoUnsignal }` as a protected member and a constructor that always set None. There was no subclass and no setter, so the AutoUnsignal branches in `wait_etc()` and `signal()` were unreachable. Zircon makes the choice in a constructor too: `AutounsignalEvent` is a subclass whose only content is the constructor passing `Flags::AUTOUNSIGNAL` (`zircon/kernel/include/kernel/event.h:115-118`). Here it is a constructor argument instead of a subclass, since a base class exists only to pass one flag.

The other choices:
- **Destroy masks and lets go of the line outside L2**, after setting Destroyed under it, as Zircon's `Destroy()` does. From step 11 the line's handler runs under the line's lock L1 and then takes L2; letting go of the line takes L1 to wait for a handler in flight, so holding L2 across it would be L2 then L1 against the handler's L1 then L2. Whether this order is right in every interleaving is DESIGN.md's open question, the user's exercise; for a virtual interrupt the three calls are empty.
- **A wait that is interrupted puts Waiting back to Idle**, as Zircon's `DoWaitForInterruptBlock()` does. Otherwise the next wait would find Waiting, left by a thread that is gone, and answer BadState for ever.
- **The pending time is a `ustl::Option<Instant>`**, where Zircon uses a `zx_time_t` whose 0 means "none pending". In Zircon a trigger at time 0 records nothing: in NeedAck it is lost, and the next wait parks. An Option has no such value, so a trigger at 0 counts. This is a departure from Zircon in one corner; the shell checks it.
- **`interrupt_create` checks options before the kind**: an unknown bit is InvArgs, and a create without Virtual names a physical line, which answers NotSupported until step 11 rather than a stand-in. A virtual interrupt needs no resource, so `resource` and `line` are not read (Zircon `sys_interrupt_create()`, `zircon/kernel/lib/syscalls/driver.cc:199-206`).

**Change**
The event's mode, before and after:

```cpp
// before: protected, and nothing could set it
enum class Flags { None, AutoUnsignal = BIT(0) };
Event() : m_canary(this), m_result(), m_flags(Flags::None), m_wq() {}

// after
enum class Options : u32 { None = 0, AutoUnsignal = BIT(0) };
Event() : Event(Options::None) {}
explicit Event(Options const options) : m_canary(this), m_result(), m_options(options), m_wq() {}
```

The two halves of a wait (`InterruptDispatcher::begin_wait()`, abridged):

```cpp
Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
switch (m_state) {
    case State::Destroyed: return Status::Cancelled;
    case State::Triggered:
        m_state = State::NeedAck;
        take_timestamp_locked(out_timestamp);
        m_event.unsignal();      // the latched signal of what was just taken
        return Status::Ok;
    case State::NeedAck:         // this wait is the ack
        if (m_timestamp.is_some()) { take_timestamp_locked(out_timestamp); m_event.unsignal(); return Status::Ok; }
        if (!!(m_flags & Flags::UnmaskPrewait)) { unmask_interrupt(); }
        break;
    case State::Idle: break;
    case State::Waiting: return Status::BadState;
}
m_state = State::Waiting;
return ustl::NONE;               // the caller parks on m_event, outside the lock
```

`trigger()` and `destroy()` signal under L2 with preemption off (`IrqSaveAndPreemptDisable`), so the reschedule a wakeup asks for waits until the lock is dropped, as Zircon's `Trigger()` and `Destroy()` hold an `AutoPreemptDisabler`. `on_zero_handles()` calls `destroy()`, so closing the last handle ends the interrupt.

The handle's default rights are Zircon's `ZX_DEFAULT_INTERRUPT_RIGHTS`: Basic, Io and Signal. `interrupt_wait` asks for Wait, `interrupt_trigger` for Signal, and `interrupt_destroy` for none, as in Zircon's `sys_interrupt_*`.

Deviation from the plan: PLAN step 2's check has "two threads trigger and wait in turn" and "destroy wakes a waiter" among the shell's checks. A program cannot start a second thread yet: `thread_create` makes one, but only `process_start` starts one, as the first thread of a new process. So the shell checks what one thread can see, and the parked paths are a kernel test, `interrupt.test`, on kernel threads and the same dispatcher calls the system calls make. A repair item records the shell checks still owed.

**Reference**
- Zircon `InterruptDispatcher::WaitForInterrupt()`, `BeginWaitForInterrupt()`, `DoWaitForInterruptBlock()`, `Trigger()`, `Destroy()`, `on_zero_handles()` (`zircon/kernel/object/interrupt_dispatcher.cc`), and `InterruptState` and the four pure virtuals in `interrupt_dispatcher.h:21-27`, `:60-63`.
- Zircon `VirtualInterruptDispatcher::Create()` and its empty `MaskInterrupt()`, `UnmaskInterrupt()`, `DeactivateInterrupt()`, `UnregisterInterruptHandler()` (`zircon/kernel/object/virtual_interrupt_dispatcher.cc`).
- Zircon `sys_interrupt_create()`, `sys_interrupt_wait()`, `sys_interrupt_trigger()`, `sys_interrupt_destroy()` (`zircon/kernel/lib/syscalls/driver.cc`).
- Zircon `AutounsignalEvent` and `Event::Unsignal()` (`zircon/kernel/include/kernel/event.h`).

**Verification**
- `boot_check.py`: dbg PASS (29 passed, 1 skipped), rel PASS (26 passed, 3 skipped). Both images print `shell: interrupt checks passed`: unknown option InvArgs, physical line NotSupported, two triggers before a wait give one interrupt with the first time, a trigger before the ack is taken by the next wait at once, a trigger at time 0 counts, missing rights AccessDenied, a channel BadType, an unwritable time InvArgs with the interrupt taken, and after destroy Cancelled for wait and trigger, Ok for a second destroy.
- `kshell_run.py --out out/bringup.x86 interrupt.test` on dev.dbg_1: `interrupt.test: ok -- 12 checks`. A trigger wakes a parked waiter with the trigger's time; a second wait while one is parked answers BadState; destroy wakes the parked waiter with Cancelled; 200 rounds of ping-pong on CPUs 0 and 1 in 1611 ms with 0 bad. `interrupt.test 1000`: 1000 rounds in 7878 ms, 0 bad.
- An earlier run of the test, which gave the turns 10 seconds in all, failed a correct run: 500 rounds took about 48 s. The test now fails only when no wait or trigger has finished for 10 s, which is what a lost wakeup looks like.
- clang-tidy on the new and changed kernel sources and on `shell_tests.cpp`: nothing on the lines this change wrote.
- Not tested: a physical line (step 11), a port (step 4), the lock order (lockdep cannot be switched on yet, `docs/repairs/260917/kernel-task-sched.md` item 6).

**Deliberately not done**
- Zircon's `ZX_VIRTUAL_INTERRUPT_UNTRIGGERED` signal, which lets a thread wait for a virtual interrupt to have been taken: nothing asks for it yet.
- `TIMESTAMP_MONO` and `WAKE_VECTOR`: there is one clock and no suspend.
- `object_get_info` for an interrupt (`ZX_INFO_INTERRUPT`): there is no topic for it yet.
- Bind, ack and ports: step 4. Physical lines, REMAP and the IRQ resource: steps 9 and 11.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Walk through the four steps in which a latched signal left standing makes the second `interrupt_wait` answer BadState. Which line prevents it, and why must it run under L2?
- Why would a manual-reset event make the synchronous wait spin, and why would no event at all lose an interrupt?
- Why does `destroy()` call the hardware operations outside L2, and what does a handler that runs after Destroyed is set do?

## Title: Ports wait on a counting semaphore, so that an interrupt can post one (2026-10-04)

**Files**
- `kernel/include/ours/sync/semaphore.hpp`, `kernel/kernel/sync/semaphore.cpp` — `sync::Semaphore` gets a count: `post()` hands its unit to the first waiter or banks it, `wait()` takes a banked unit or blocks, up to a deadline. It was a stub: `post()` did nothing and `wait()` answered Unimplemented.
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` — `m_sema` replaces `m_ready`. `queue_user()` and `queue_observer_packet()` post after releasing the port lock; `dequeue()` takes a unit before it takes the lock, and loops when the queue was emptied under it; `on_zero_handles()` no longer unsignals anything.
- `kernel/commands/semaphore_test.cpp`, `kernel/commands/BUILD.gn` — new: `semaphore.test`.
- `docs/design/260923/userspace-interface.md`, `docs/design/260924/userspace-filesystem.md` — "Revised 2026-10-04" notes: the K26 reasoning that a Semaphore was not needed is superseded.
- `docs/repairs/261004/kernel-sync.md` — new: the atomic fast path this semaphore leaves out.

**Symptom**
Latent, cannot be triggered today. It would be the moment step 4 of `.agents/tasks/microkernel-interrupts/PLAN.md` queues an interrupt packet from an interrupt handler: the port's waiters were woken by a manual-reset `sync::Event` whose state was kept equal to "the queue is not empty" only under `m_port_lock`, a Mutex, which an interrupt handler cannot take. Today every packet is queued from thread context, so nothing misbehaves; the cost visible today is that one packet woke every waiter on the port, and all but one went back to sleep.

**Cause**
The old port used a level where it needed a count, and a level can only be kept true by whoever can see everything it summarises.

The objects, before this change:
- `m_packets`, the queue, under `m_port_lock` (a `sync::Mutex`).
- `m_ready`, a manual-reset `sync::Event`. "Signalled" was meant to be the same fact as "`m_packets` is not empty". `queue_user()` and `queue_observer_packet()` signalled it, and `dequeue()` unsignalled it when it took the last packet, both under `m_port_lock`.
- A waiter in `dequeue()`: look at the queue under the lock; if it is empty, sleep on `m_ready` with no lock held; on waking, look again.

That works for exactly one queue under exactly one lock. The event is a summary of the queue, and the summary is only correct if every change to the queue and to the event happens inside one critical section. K26 chose it deliberately for that reason (`docs/design/260924/userspace-filesystem.md`, the lock-order section): posting after the lock, as Zircon does, would leave a manual-reset event signalled over an empty queue, and every waiter would spin on it.

Step 4 adds a second queue. An interrupt packet lives inside its interrupt object and is queued from the interrupt handler, so it goes on a queue of its own under a spinlock, L3, taken with interrupts off. Now the summary has to cover two queues under two locks. The worst interleaving:
1. CPU 0, in the interrupt handler: queue the interrupt packet under L3, signal `m_ready`. The event was already signalled, so nothing changes.
2. CPU 1, a waiter in `dequeue()`: take `m_port_lock`, take the last ordinary packet, see `m_packets` empty, unsignal `m_ready`.
3. The next `dequeue()`: the event is unsignalled, so it sleeps, with an interrupt packet sitting on the L3 queue. Nothing will wake it until some unrelated packet arrives. For a driver, that is a lost interrupt: the line stays masked (MASK_POSTWAIT) and the device goes silent.

Step 2 is the bug: it unsignalled on the evidence of one queue. Fixing it within the event model means checking both queues, under both locks, before every unsignal (D6 option a2), which puts L3 under the Mutex on every dequeue. The other way out, D6 option b, is to never post from the interrupt at all and hand the work to a Dpc thread, which costs a thread switch per interrupt.

A counting semaphore removes the summary instead of guarding it. It counts events, not a state:
- A producer, after its packet is on a queue and the queue's lock is released, posts one unit.
- A consumer takes one unit before it looks at any queue, then takes the queue's lock and pops.

Nobody ever "unsignals", so no consumer needs to see both queues at once. The units on the semaphore equal the packets queued, plus the packets taken off without a unit (by `on_zero_handles()` today; by `RemoveInterruptPacket` and `port_cancel` later). A consumer holding a unit therefore finds a packet, or finds that one was taken away, and goes back to the semaphore. Posting after the lock loses nothing: a consumer that arrives between the push and the post finds no unit and sleeps, and the post then wakes it. And since the semaphore takes none of the port's locks, the interrupt handler can post it, which is D6 (a1).

The semaphore itself has to be safe in IRQ context, and has one race to settle.
- **IRQ context.** `post()` takes only the wait queue's spin lock, with interrupts off, and `Scheduler::unpark_thread()`, which takes the woken thread's PI lock and the run-queue lock and only asks for a reschedule (`reschedule_mask()`). This is the shape of `Event::signal()`, which already runs from the tick.
- **A post racing a timeout.** The count is never negative, and a unit handed to a waiter never touches the count. So "a thread is queued only while the count is zero" holds at every release of the queue lock. A waiter that times out or is killed takes itself off the queue under the same lock (`WaitQueue::handle_timeout()`). Whichever of the post and the timeout gets the lock first owns the outcome. If the post does, the waiter is off the queue with Ok and returns with its unit. If the timeout does, the post finds the queue without it and banks the unit.
- **Zircon's count does go negative.** There, a blocked waiter is counted as -1, which lets a post or a wait skip the lock when nobody blocks. Then a waiter that times out leaves the count wrong, and the next `Post()` repairs it under the lock ("There are no waiters to wake. They must have timed out or been interrupted."). That is an optimisation with a subtler invariant. This change takes Linux's shape, where every call takes the lock, and records the fast path as a performance item to be measured (`docs/repairs/261004/kernel-sync.md`, item 1).

**Change**
`sync::Semaphore::post()`, after:

```cpp
arch::AutoInterruptDisabler const irq_disabler;
Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};
auto woken = m_wq.lock_for_notify_one<Thread::UnparkList>(Status::Ok);
if (!woken) {
    m_count += 1;
    return;
}
guard.unlock();
Scheduler::unpark_thread(ustl::move(*woken));
```

`wait_etc()` checks the count and enqueues under one hold of the queue lock, inside the thread's PI lock, as `Event::wait_etc()` does. A deadline already passed makes it a poll: `park_thread_preamble_locked()` answers TimeOut before anything is queued.

The port, before:

```cpp
// queue_user(), under m_port_lock
m_packets.push_back(*queued.into_raw());
m_ready.signal();

// dequeue()
for (;;) {
    { Guard guard{m_port_lock}; /* pop; if now empty, m_ready.unsignal() */ }
    if (taken) { return Ok; }
    if (deadline == 0) { return TimeOut; }
    m_ready.wait(infinite);
}
```

After:

```cpp
// queue_user()
{ Guard guard{m_port_lock}; m_packets.push_back(*queued.into_raw()); }
m_sema.post();

// dequeue()
for (;;) {
    if (auto const status = m_sema.wait(wait_deadline); status != Status::Ok) {
        return status;
    }
    { Guard guard{m_port_lock}; /* pop, if there is a packet */ }
    if (taken) { return Ok; }
}
```

A poll (`deadline == 0`) waits with `Deadline::at(TimePoint<>{})`, which the semaphore answers at once. `queue_observer_packet()` posts after the lock as well, and touches nothing of the observer after it: a waiter may already have freed it with its packet. The port stays alive across the post because `on_match()` holds a reference for the length of the call.

Two behaviours change, both as in Zircon:
- A packet wakes one waiter, not all of them.
- A poll that runs between a producer's unlock and its post answers TimeOut although the packet is queued. The producer's call has not returned yet, so the poll is ordered before it; the packet is there for the next wait.

Rejected:
- D6 (a2), keeping the event and checking both queues under both locks before unsignalling: every dequeue would take the interrupt queue's spinlock under the Mutex, and the summary would still need re-proving whenever a third packet source appeared.
- D6 (b), posting from a Dpc: a thread switch per interrupt, and `Destroy` would have to cancel the Dpc first. It caps the latency later.
- Zircon's negative count, for now: see the repair item.

**Reference**
- Zircon `Semaphore::Post()` and `Semaphore::WaitInternal()` in `zircon/kernel/kernel/semaphore.cc`.
- Zircon `PortDispatcher::QueueAndRemoveObserver()`, `PortDispatcher::QueueInterruptPacket()` and `PortDispatcher::Dequeue()` in `zircon/kernel/object/port_dispatcher.cc`: post after the lock, wait before it, and loop when "the packet must have been removed before we were able to dequeue".
- Linux `up()` and `___down_common()` in `linux/kernel/locking/semaphore.c`: the count never negative, the unit handed straight to the first waiter, every call under one IRQ-safe spin lock.

**Verification**
- `boot_check.py`: dbg PASS (28 passed, 1 skipped), rel PASS (25 passed, 3 skipped). Both images print `shell: port checks passed` and `shell: wait_async checks passed`, unchanged.
- `semaphore.test` on the dbg image: ok, 18 checks.
  - 8 posts with nobody waiting are banked, 8 polls take one each, and the 9th answers TimeOut.
  - A wait with nothing banked stays parked until another thread posts, and the unit goes to it rather than to the count.
  - A 5 ms wait that nothing posts answers TimeOut, after 11081 us and 28598 us in two runs on a loaded host (never before the deadline), leaves the queue, and a post after it is banked.
  - A post from a timer handler, in interrupt context, wakes the parked waiter.
  - With 4 waiters parked, 3 posts wake exactly 3, and the 4th post wakes the last.
- `port.test` on the dbg image: ok, 200 rounds of 64 raced and 8 stranded waits on CPUs 0 and 1; 3464 observers torn down, 146 callbacks yielded to the tear-down, 0 bad rounds; first, 1024 waits armed through a handle table before NoResource.
- The test fails when it should: with `post()` changed to always bank and never wake, `semaphore.test` reports FAILED, 6 of 18 checks (the handoff, the interrupt post and the crowd), and does not hang.
- clang-tidy on `semaphore.cpp`, `port_dispatcher.cpp` and `semaphore_test.cpp`: nothing reported on these files or their headers. `scripts/format_cpp.py --check`: clean.
- Not measured: the cost per post and per wait (repair item 1 of `kernel-sync.md`).

**Deliberately not done**
- The interrupt queue, the L3 spinlock and serving interrupt packets first: step 4.
- The atomic fast path: a performance change, which waits for a benchmark and the user's decision (`docs/repairs/261004/kernel-sync.md`, item 1).
- Taking back units on `on_zero_handles()`: Zircon does not either. A waiter holding a stale unit finds the queue empty and goes back to the semaphore, once per stale unit.
- The concurrency argument CLAUDE.md asks the user to write before a lock decision: skipped for this MVP run by the user's instruction, and open in the design's Open questions.
- lockdep: no image links it (`docs/repairs/260917/kernel-task-sched.md`, item 6).

**How it was found**
None: defect 12 of `.agents/tasks/microkernel-interrupts/DESIGN.md`, found by reading the port before designing interrupt packets.

**The user's prediction**
None.

**Three questions you should be able to answer**
- With a manual-reset event over two queues under two locks, which interleaving loses a wakeup, and why does a counting semaphore not need to see both queues to avoid it?
- Why is it safe for the producer to post after releasing the queue's lock, and what does a consumer do when it holds a unit but finds the queue empty?
- When a post and a waiter's timeout race, which lock decides who gets the unit, and why does Zircon's negative count need a repair step that this semaphore does not?

## Title: Interrupts bound to ports: interrupt_bind, interrupt_ack and ports made with BindToInterrupt (2026-10-04)

**Files**
- `system/public/ours/port.hpp` — `PacketType::Interrupt = 7`, the `PacketInterrupt` payload in the packet's union, and `PortCreateOptions::BindToInterrupt = 0x1`
- `system/public/ours/interrupt.hpp` — `InterruptBindOptions`: Bind (0) or Unbind (1)
- `system/public/ours/syscalls.gni` — new calls `interrupt_bind` and `interrupt_ack`; `port_create`, `interrupt_wait`, `interrupt_trigger` and `interrupt_destroy` document what binding changes
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` — `PortInterruptPacket`; the interrupt queue `m_interrupt_packets` under its own spin lock `m_interrupt_lock` (L3); `create()` takes the options; `queue_interrupt_packet()`, `remove_interrupt_packet()`; `dequeue()` serves the interrupt queue first; the destructor checks it is empty
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — the bound port `m_port` and the embedded `m_port_packet`; `bind()`, `unbind()`, `ack()` and `send_packet_locked()`; the `MaskPostwait` flag; `trigger()` sends a packet when bound, `begin_wait()` refuses a bound interrupt, `destroy()` takes a queued packet back, `bind()` unsignals the synchronous wait's event
- `kernel/lib/syscall/src/interrupt.cpp`, `kernel/lib/syscall/src/port.cpp` — the two calls; `port_create` hands its options to the port
- `kernel/commands/port_test.cpp` — `PortDispatcher::create()` takes the options
- `kernel/commands/interrupt_test.cpp` — a bound interrupt's packet wakes a thread parked in the port's dequeue; an interrupt triggered unbound, then sent by a bind, read, acked and unbound leaves no signal latched, so the next wait parks; `park_waiter()` takes a step to run on the interrupt before the waiter starts
- `examples/shell/shell_tests.cpp` — new `check_interrupt_port()`, on both images; `check_port()`'s unknown option is now 2, since 1 is BindToInterrupt
- `docs/design/260923/userspace-interface.md` — section 4.8: the IRQ path's locks in the total order, and the interrupt object as the exception to hard rule 3
- `.agents/skills/ours-kernel-design/references/task.md` — the same exception, with its reason
- `docs/repairs/261004/object.md` — item 1 notes that a bind can leave the second of two waiters parked through a destroy

**Symptom**
None, a planned change: step 4 of `.agents/tasks/microkernel-interrupts/PLAN.md`. Until now an interrupt could only be taken by a thread blocked in `interrupt_wait`, so a driver had to give a thread to every interrupt it owns and could not wait on an interrupt and a channel at once. A port is the one place a driver waits on everything; this step lets an interrupt deliver there, which is how step 12's PS/2 driver will run its loop.

**Cause**
The part worth learning is why the packet is embedded in the interrupt object, why there is exactly one, and what that forces on destroy and unbind.

The objects:
- The interrupt object's state machine (step 2), under its spin lock L2: `m_state`, `m_timestamp` (the first interrupt not yet delivered, an Option), and now `m_port`, a reference to the bound port, and `m_port_packet`, a `PortInterruptPacket` (`key`, `timestamp` and an intrusive list hook) that is a member of the interrupt object.
- The port's interrupt queue `m_interrupt_packets`, an intrusive list of those embedded packets, under `m_interrupt_lock` (L3), a spin lock taken with interrupts off; and `m_sema`, the counting semaphore from step 3, posted once per packet queued on either queue.
- The reader: a thread in `port_wait`. The driver: the thread that reads the packet, quiets the device and calls `interrupt_ack`.

Why the packet cannot be allocated: from step 11 a physical line's handler sends it in interrupt context, under the line's lock L1 and the object's L2. An allocation there could fail, and the allocator's own locks are not ones an interrupt may take. So the packet already exists, inside the object, and queueing it only links a list node.

Why one packet is enough, and why that bounds the port: an interrupt that has sent a packet is in NeedAck, and sends nothing more until `interrupt_ack`. Interrupts that arrive meanwhile only record a time in `m_timestamp` if none is recorded (the first one wins), so a storm of interrupts costs one queued packet and one pending time, never a growing queue. The ack is where the next packet goes: if a time is pending, `ack()` sends it at once and the object stays in NeedAck; otherwise it goes back to Idle. The timeline the shell checks:
1. Trigger at 1001: Idle and bound, so the packet goes out with 1001, the time is cleared, and the state is NeedAck.
2. Trigger at 1002: NeedAck, so 1002 is recorded and nothing is sent.
3. `port_wait` reads the packet with 1001.
4. Trigger at 1003: a time is already pending, so 1003 folds into 1002.
5. `interrupt_ack`: NeedAck with 1002 pending, so the next packet goes out with 1002, and the state stays NeedAck.

A packet carries the time of the first interrupt after the previous packet went out, not of the last one before the ack. The first version of the shell check expected the ack to send the trigger that came after the read (2001 in that version); it got 1002, which is right. That is how the test came to say it in these terms.

Why a packet already queued is not queued twice: the list hook is the only record of where the packet is, and linking a node that is already linked would corrupt the list. Two paths reach a send while the packet is still queued, unread: an ack that comes before the reader took the packet, which answers BadState; and a trigger after such an ack moved the object to Idle. In both the pending time is cleared and the interrupt folds into the packet still queued, which the reader then sees: one packet stands for every interrupt up to the moment it is read.

Why a separate queue under a spin lock rather than `m_packets` under the port's Mutex: a Mutex sleeps, and an interrupt handler cannot. Step 3's semaphore is what lets two queues under two locks share one wakeup: each producer queues under its queue's lock and posts after letting it go, and a waiter takes a unit before it looks at either queue. The two locks are never held together; a dequeue looks at the interrupt queue, lets L3 go, and only then takes the Mutex. The interrupt queue is served first because an interrupt left waiting is a device left waiting, its line possibly masked until the ack.

Why destroy and unbind must take the packet back: the port's list holds a pointer into the interrupt object, and nothing else. If the object were freed while its packet sat on a port, the next `port_wait` would read freed memory and the list would be corrupt. So `destroy()` and `unbind()` call `remove_interrupt_packet()` under L2. `destroy()` always runs before an interrupt that was ever bound can be freed: binding takes a handle, and closing the last handle calls `on_zero_handles()`, which calls `destroy()`. Two checks back this up: the hook's safe-link destructor traps if the object is freed while its packet is linked, and `~PortDispatcher` asserts the interrupt queue is empty, which holds because every bound interrupt keeps a reference to its port until it is freed. `destroy()` answers NotFound when the object was in NeedAck and its packet was no longer queued: the packet was delivered and the interrupt is gone before its ack.

Why a bind unsignals the synchronous wait's event, found at review: the event is auto-unsignal, so a trigger with no one parked latches its signal for the next `wait()`, and every path in `begin_wait()` that takes an interrupt unsignals it under L2, so the latch never outlives the interrupt it stood for. A bind is one more path that takes an interrupt: one triggered while unbound is Triggered with the signal latched, and the bind sends it to the port. Without the unsignal the latch stood through the packet, its read, its ack and an unbind; the first `wait()` after that set Waiting, parked, woke at once on the latch, came round to `begin_wait()`, found the Waiting it had set itself, and answered BadState instead of parking until the next trigger. The same happens to a time pending in NeedAck when the bind lands, since that trigger signalled too and the ack now sends it. So `bind()` unsignals under L2 whatever the state: nothing signals the event while bound, and from the bind on nothing stands latched on it either. The event's queue lock under L2 is the order `begin_wait()` already takes.

Why an unbind puts a queued packet's interrupt back to Triggered: a packet nobody read was never delivered. Dropping it would lose the interrupt; restoring Triggered with the packet's time hands it to the next wait or the next bind, which sends it at once. A packet already read stays delivered, and the object stays in NeedAck, owed its ack, which is now a wait.

The locking details:
- The key is written by `bind()` under L2 alone, while the packet is on no queue; the port reads it under L3 after the packet was queued under L3, so L3 orders the two.
- `unbind()` moves the port reference into a local declared before its guard, so that dropping what may be the port's last reference, and so running its destructor, happens after L2 is released.
- `destroy()` keeps the binding, so that a later `interrupt_ack` or unbind answers Cancelled rather than as if the interrupt had never been bound.
- `bind()`, `ack()` and `trigger()` hold L2 with preemption off, because a packet they send posts the port's semaphore, which may wake a thread; `unbind()` sends nothing and holds L2 with `IrqSave` alone.
- The order on this path is L2 → L3, and the semaphore's post runs after L3 is released but inside L2. That is the design's L1 → L2 → {LI, L3}, now written into section 4.8 of the accepted design, together with the exception to its hard rule 3 that interrupt-context producers only queue a Dpc (D6 (a1)).

The comments the earlier attempt at this step wrote named the system these functions follow; the comment rule of the same day keeps that comparison here, under Reference, and the code says what each piece is in this kernel's terms.

**Change**
The port's dequeue, before and after (abridged):

```cpp
// before
for (;;) {
    if (auto const status = m_sema.wait(wait_deadline); status != Status::Ok) { return status; }
    { Guard guard{m_port_lock}; /* pop from m_packets */ }
    if (taken != nullptr) { free_packet(taken); return Status::Ok; }
}

// after
for (;;) {
    if (auto const status = m_sema.wait(wait_deadline); status != Status::Ok) { return status; }
    if (can_bind_to_interrupt() && dequeue_interrupt_packet(out)) {   // under L3, then let go
        return Status::Ok;
    }
    { Guard guard{m_port_lock}; /* pop from m_packets */ }
    if (taken != nullptr) { free_packet(taken); return Status::Ok; }
}
```

Sending, and the ack (abridged):

```cpp
auto InterruptDispatcher::send_packet_locked() -> bool {        // under L2
    auto const queued = m_port->queue_interrupt_packet(&m_port_packet, *m_timestamp);
    if (!!(m_flags & Flags::MaskPostwait)) { mask_interrupt(); }
    m_timestamp.reset();
    return queued;                                               // false: folded into the queued one
}

// trigger(), after recording the time
if (m_port) {
    if (m_state != State::NeedAck) { auto _ = send_packet_locked(); m_state = State::NeedAck; }
    return Status::Ok;
}

// ack()
if (!m_port) { return Status::BadState; }
if (m_state == State::Destroyed) { return Status::Cancelled; }
if (m_state != State::NeedAck) { return Status::Ok; }
if (!!(m_flags & Flags::UnmaskPrewait)) { unmask_interrupt(); }
if (m_timestamp.is_some()) {
    if (!send_packet_locked()) { return Status::BadState; }      // the last is still unread
    return Status::Ok;                                           // stays NeedAck
}
m_state = State::Idle;
```

The ABI: `PacketType::Interrupt` is 7, `PortCreateOptions::BindToInterrupt` 0x1, `InterruptBindOptions` Bind 0 and Unbind 1, and the payload is a 32-byte `PacketInterrupt` whose first field is the time, with the union's other members at offset 16. Packet types 2 to 6 stay out of use. `interrupt_bind` asks for Read on the interrupt and Write on the port, and answers BadType for a port made without BindToInterrupt; `interrupt_ack` asks for Write.

`bind()` after the review, unsignalling the event:

```cpp
// before
m_port = ustl::move(port);
m_port_packet.key = key;
if (m_state == State::Triggered) { auto _ = send_packet_locked(); m_state = State::NeedAck; }

// after
m_port = ustl::move(port);
m_port_packet.key = key;
m_event.unsignal();                    // the port gets the interrupts the latch stood for
if (m_state == State::Triggered) { auto _ = send_packet_locked(); m_state = State::NeedAck; }
```

Rejected:
- Signalling the event in `destroy()` of a bound interrupt as well, which would wake a second waiter left parked by the race of repair item 1: it covers one corner of a race whose fix is the user's to choose, and it would wake that waiter with BadState rather than Cancelled. The repair item now records this facet.
- A result for the ack that says whether it sent the next packet: nothing reads it. It matters only to a signal raised while a virtual interrupt is not triggered, which step 2 left out.

**Reference**
- Zircon `InterruptDispatcher::Bind()`, `Unbind()`, `Ack()` and `AckInternal()`, `SendPacketLocked()`, `Trigger()`, `Destroy()` and `BeginWaitForInterrupt()` in `zircon/kernel/object/interrupt_dispatcher.cc`. The unsignal in `bind()` departs from Zircon's `Bind()`, which sends a TRIGGERED interrupt with `SendPacketLocked()` and leaves `event_` signalled; read against `BeginWaitForInterrupt()`, whose WAITING case answers `ZX_ERR_BAD_STATE`, the next wait after an unbind there meets the same latch (not run); `AckInternal()`'s `PostAckState::Retriggered` is read only by `VirtualInterruptDispatcher::Ack()` (`virtual_interrupt_dispatcher.cc`), for its UNTRIGGERED signal.
- Zircon `PortDispatcher::Create()`, `QueueInterruptPacket()`, `RemoveInterruptPacket()` and `Dequeue()` in `zircon/kernel/object/port_dispatcher.cc`, and `PortInterruptPacket` and `can_bind_to_interrupt()` in `zircon/kernel/object/include/object/port_dispatcher.h`.
- Zircon `sys_interrupt_bind()` and `sys_interrupt_ack()` in `zircon/kernel/lib/syscalls/driver.cc`: the same rights, and `ZX_ERR_WRONG_TYPE` (BadType here) for a port that cannot take interrupts.
- The ABI values are Zircon's: `ZX_PKT_TYPE_INTERRUPT`, `zx_packet_interrupt_t` and `ZX_PORT_BIND_TO_INTERRUPT` in `zircon/system/public/zircon/syscalls/port.h`, `ZX_INTERRUPT_BIND` and `ZX_INTERRUPT_UNBIND` in `types.h`. Packet types 2 to 6 are `ZX_PKT_TYPE_SIGNAL_REP`, which Zircon has retired, and the hypervisor's `ZX_PKT_TYPE_GUEST_BELL`, `GUEST_MEM`, `GUEST_IO` and `GUEST_VCPU`.
- The same shape elsewhere, for comparison: Linux VFIO's INTx is AUTOMASKED and unmasked by the user (`vfio_pci_intx_unmask_handler()` in `drivers/vfio/pci/vfio_pci_intrs.c`), and UIO unmasks when the user writes 1 (`uio_pdrv_genirq_irqcontrol()` in `drivers/uio/uio_pdrv_genirq.c`). The ack here is that unmask.

**Verification**
- `boot_check.py`: dbg PASS (30 passed, 2 skipped), rel PASS (27 passed, 7 skipped). Both images print `shell: interrupt port checks passed`, and still `shell: interrupt checks passed`, `shell: port checks passed` and `shell: wait_async checks passed`.
- `check_interrupt_port()` checks, from one thread with polls: unknown bind option InvArgs; a port without BindToInterrupt and a port for the interrupt BadType; no Read on the interrupt, no Write on the port, and no Write for the ack AccessDenied; ack while unbound BadState; bound, a wait BadState and a second bind AlreadyBound; the five-step timeline above; a second ack Ok with nothing sent; an interrupt packet read before a port_queue packet queued ahead of it; an ack before the read BadState, with the next interrupt folded into the unread packet; unbind takes a queued packet back, a wait then takes its time, and a second unbind NotFound; a bind sends a pending interrupt at once; destroy with a packet queued leaves the port empty, then ack and bind Cancelled; destroy after the packet was read NotFound.
- The check fails when it should: with `destroy()` changed to leave the packet on the port, both images report `shell: port_wait(destroyed) = Ok (expected TimeOut)` and `shell: interrupt port checks FAILED`.
- `kshell_run.py` on dev.dbg_1: `interrupt.test: ok -- 18 checks` (a bound interrupt's trigger wakes a thread parked in the port's dequeue with the Interrupt packet, its key and time, and a destroy after that answers NotFound; 200 ping-pong rounds, 0 bad); `port.test: ok` (200 rounds, 0 bad); `semaphore.test: ok -- 18 checks`.
- clang-tidy on every changed kernel source and on `shell_tests.cpp` and `interrupt_test.cpp`: nothing on the lines this change wrote. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean.
- At review, the latch a bind left: a new `interrupt.test` check triggers a virtual interrupt unbound, binds it, reads the packet with a poll, acks and unbinds, then parks a waiter on another CPU and triggers again. Before the unsignal it failed, `interrupt.test: FAIL  a wait with nothing pending parks` and `FAILED -- 1 of 19 checks`: the waiter answered without parking. With it, `interrupt.test: ok -- 22 checks`, the waiter woken by the trigger with its time; `port.test: ok`, `semaphore.test: ok -- 18 checks`. `boot_check.py`: dbg PASS (30 passed, 2 skipped), rel PASS (27 passed, 7 skipped, 2/2 required), both printing `shell: interrupt port checks passed`. The shell cannot catch this one: from one thread, a wait that parks never returns.
- Not tested: the send from interrupt context through a physical line's handler (step 11; `semaphore.test` already posts from a timer interrupt), `MaskPostwait` and `UnmaskPrewait`, which no kind sets yet, and the lock order, which lockdep cannot check until it links (`docs/repairs/260917/kernel-task-sched.md` item 6).

**Deliberately not done**
- The handler a physical line calls, and the first kind that sets `MaskPostwait` and `UnmaskPrewait`: step 11.
- Taking back semaphore units when a packet is taken back: a waiter holding a stale unit finds the queues empty and waits again, as for `on_zero_handles()`.
- Emptying the interrupt queue in the port's `on_zero_handles()`: a packet there belongs to an interrupt that still holds the port, and goes when that interrupt is destroyed or unbound.
- The concurrency argument CLAUDE.md asks the user to write before a lock decision: skipped for this run by the user's instruction, and open in the design's Open questions.

**How it was found**
None, a planned step. The first run of the shell check failed on its own expectation, not on the kernel, as told under Cause. The latch a bind left was found at review, by reading `bind()` against the comment on `begin_wait()` ("each answer that takes an interrupt unsignals m_event under the lock"), and confirmed by the `interrupt.test` check above failing before the fix.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the interrupt's packet be embedded in the interrupt object, and what would go wrong if `destroy()` did not take a queued packet back off the port?
- Triggers arrive at 1001, 1002 and 1003, with a `port_wait` after the first and an ack after the third: which packets does the port deliver, with which times, and why is it not 1003?
- Why can a port's Mutex and its interrupt-queue lock never be held together, and what lets one semaphore wake waiters for two queues under two different locks without losing a wakeup?
