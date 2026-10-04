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
