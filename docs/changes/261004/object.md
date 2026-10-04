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
