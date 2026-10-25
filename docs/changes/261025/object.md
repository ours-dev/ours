# object changes

## Title: a port queues packets another kernel object owns, and hands each back through free() (2026-10-25)

**Files**
- `kernel/object/include/ours/object/port_dispatcher.hpp` — new `PortPacketOwner` (`free(packet)`); `QueuedPacket` gains `origin` (User, Observer, Kernel) and its `owner` is a `PortPacketOwner`; `PortObserver` is one; new `PortDispatcher::queue()` and `cancel_queued()`
- `kernel/object/port_dispatcher.cpp` — the two new calls; every packet off the queue goes back through its owner, or to the allocator for a port_queue one; the counts follow `origin`
- `kernel/commands/port_test.cpp` — the owned-packet checks (7 new), run before the rounds

**Symptom**
None, step 6 of `.agents/tasks/user-pager/`. A pager's proxy (step 9) sends each page request to the pager's port while it holds the VMO's and the PageSource's locks, where nothing may be allocated. A port could only queue a packet it allocated (`queue_user()`) or one inside a `PortObserver`; nothing else could put a packet on it.

**Cause**
`QueuedPacket::owner` was a `PortObserver *`, and `free_packet()` knew exactly two cases: a null owner meant a packet the port allocated, freed to the allocator; anything else was an observer, and taking the packet freed the observer. The counts leaned on the same test (`owner == nullptr` meant a user packet, anything else an observer). A third kind of packet, embedded in some other kernel object and reused for every send, had nowhere to go: there was no way to queue it, to tell its owner the port was done with it, or to take it back.

**Change**
An owner interface, as Zircon's `PortAllocator` is for its packets:

```cpp
class PortPacketOwner {
  public:
    virtual auto free(QueuedPacket *packet) -> void = 0;   // with no lock of the port's held
};
```

Every packet the port takes off its queue goes back the same way: `packet->owner->free(packet)` if it has an owner, the allocator otherwise. `PortObserver` is now an owner too, whose `free()` frees the observer, so the observer path is the general path and not a special case. `QueuedPacket::origin` (User, Observer, Kernel) says only what the packet is counted in: User in `m_user_packets`, Observer in `m_live_observers`, Kernel in nothing, since an owner has one packet in flight at most.

`queue(packet)` puts a Kernel packet on the queue and posts the semaphore after the lock, as `queue_user()` does. Once the port has no handles it answers BadHandle and keeps nothing: `free()` is not called, the caller still has the packet, and has to treat it as delivered (the design's fix over Zircon, so that a proxy whose last `Complete` could not be queued does not wait for a `free()` that never comes).

`cancel_queued(packet)` takes a packet back if it is still queued: true, the caller has it again and no `free()` follows; false, `dequeue()` already took it or the port dropped it, and `free()` has run or will. The false case includes a port that has lost its handles: `on_zero_handles()` moves the queue onto a list of its own in the same hold of the lock that sets ZeroHandles, then walks it without the lock, so a packet still linked then is on that list, not on `m_packets`, and erasing it from `m_packets` would corrupt both lists. The flag says which list it is on, and the tear-down frees it.

`free()` always runs with the port's lock let go (`dequeue()` after its guard, `on_zero_handles()` after the move), so an owner may queue its packet again from inside `free()`, which is what the proxy will do to send the next request it kept.

Rejected: a callback from the port with the lock held, which would make "queue the next one from free()" a self-deadlock; and allocating a packet per send, which cannot be done under the VMO's lock.

**Reference**
Zircon `PortAllocator` (`object/include/object/port_dispatcher.h`) with `Free(PortPacket*)`, called by `PortDispatcher::Dequeue()` and `on_zero_handles()` once the packet is off the queue; `PortDispatcher::Queue()`, and `CancelQueued(PortPacket*)` returning whether it took the packet off. `PagerProxy` embeds one packet and frees nothing in `Free()`; it marks the packet free and sends the next queued request. Where it differs: Zircon's `Queue()` frees nothing when the port has no handles and returns `ZX_ERR_BAD_HANDLE` too, but its pager proxy then waits for the `Complete` packet's `Free()`; the design here has the caller treat the refused packet as delivered.

**Verification**
- `port.test`, 7 new checks before the rounds: a queued packet is read with its key and handed back once; an owner queues the packet again from inside `free()` and it is read again (the port's lock is let go); `cancel_queued()` takes a queued packet back with no `free()`, only once, and a poll then finds nothing; a packet already read cannot be cancelled; the port's last handle going drops a queued packet and hands it back once; after that, `queue()` is BadHandle with no `free()` and `cancel_queued()` false.
- The rounds still pass (50: 1206 observers torn down, 50 callbacks yielded to the tear-down, 0 bad rounds): observer packets go through the new owner path.
- Control runs: with Kernel packets skipped in the tear-down, 2 checks fail; with `free()` called under the port's lock in `dequeue()`, the owner's re-queue from `free()` takes the same lock and `port.test` hangs past its 30 s limit. Restored, all pass.
- `pagesource.test`, `vmosnapshot.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- The page request packet type and payload: step 7.
- The proxy that owns such a packet: step 9.
- A race test of `cancel_queued()` against `on_zero_handles()`: the argument above is what the flag check rests on; a test would need the cancel to land between the move and the walk.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why must free() run with the port's lock let go, and what would an owner that queues its packet again from free() do otherwise?
- Why does cancel_queued() answer false once the port has no handles, even for a packet that is still linked?
- Why does a queue() refused with BadHandle not call free(), and what does its caller have to do instead?

## Title: PagerProxy turns a pager VMO's page requests into packets on the pager's port (2026-10-25)

**Files**
- `kernel/object/include/ours/object/pager_proxy.hpp`, `kernel/object/pager_proxy.cpp` — new: `PagerProxy`, the `PageProvider` of one pager VMO and the `PortPacketOwner` of its one packet
- `kernel/object/BUILD.gn` — builds it
- `kernel/commands/pager_test.cpp` — new: `pager.test` (10 checks); `kernel/commands/BUILD.gn` builds it

**Symptom**
None, step 9a of `.agents/tasks/user-pager/` (step 9 split in two at the user's word: the proxy here, the `PagerDispatcher` that owns proxies in 9b). A VMO made over a page source had only the kernel tests' providers; nothing turned its requests into something a user pager could read.

**Cause**
None, an addition. What makes the proxy more than a few lines is what it may not do and how long it has to live:
- It sends under the VMO's and the PageSource's locks, where nothing may be allocated, so it owns one packet and reuses it (step 6). Requests that come while the packet is out wait in the proxy.
- The port holds a plain pointer to that packet while it is queued, and no reference to the proxy. So the proxy must not be destroyed while its packet may be on the port, and that can outlive the VMO: the last packet, Complete, is sent as the VMO goes.

**Change**
The objects (see the class comment): the packet; `m_active`, the request it carries; `m_pending`, the requests waiting for it; `m_complete`, a `PageRequest` that stands for the Complete packet; and `m_source`, a reference to the PageSource, which holds the proxy as its provider. The cycle keeps both alive and is cut where the port can no longer have the packet once the source is closed:

```cpp
on_close():  m_source_closed = true; if (!m_complete_pending) cut
free(pkt):   packet back; if it carried Complete, m_complete_pending = false; else send the next pending;
             if (m_source_closed && !m_complete_pending) cut
cut:         return ustl::move(m_source)   // dropped by the caller after m_lock is let go
```

The events, a Read round trip: the source raises a request and calls `send_async_request()` under its lock; the proxy fills the packet (`PacketType::PageRequest`, `PageRequestCommand::Read`, the request's offset and length) and `queue()`s it, or puts the request on `m_pending` if the packet is out. The pager's `port_wait` takes the packet, the port calls `free()` with its lock let go, and the proxy sends the next pending request in the same packet. When the source takes a request back (`clear_async_request()`: completed, or withdrawn by a waiter that gave up), the proxy takes the packet back off the port if it is still queued (`cancel_queued()`), else forgets the request and lets the coming `free()` find nothing on the packet.

On detach, Complete goes out after whatever is out, as the last packet; by then the source has taken back every request, so a Read still queued is withdrawn first.

Two choices beyond Zircon's proxy, both about a port that has lost its handles, whose `queue()` answers BadHandle and keeps nothing:
- The packet stays the proxy's. Zircon treats such a packet as queued and never answered, so the packet stays busy for good; a Complete behind it would never go out, and the proxy and source would never be freed. Here a refused Read is left to its waiter, whose wait ends by overtime or by the detach that comes with the VMO's end, and the queue drains.
- A refused Complete is as good as delivered (`m_complete_pending = false`), so `on_close()` cuts the cycle at once; Zircon waits for a `free()` that never comes.

Waiting (decision D3): `wait_on_event()` waits in rounds of `warn_every`, logs a warning after each, and gives up with TimeOut after `give_up_after`, 20 s and 300 s by default as in Zircon's boot options; a suspend does not end a wait that may not be interrupted by one (`ThreadSignals::Suspend` masked), a kill always does. The two durations are a parameter of the proxy, so that the test can use 20 ms and 60 ms.

Locks: PageSource, then the proxy's `m_lock`, then the port's lock, a leaf. `m_source` is dropped after `m_lock` is let go, since `~PageSource` takes the source's lock. In `on_close()`, called under the source's lock, the reference dropped is never the last: whoever calls `close()` holds one (the node in its destructor, `with_source()`'s error paths).

Rejected: allocating a packet per request (not under the VMO's lock); a reference from the port to the proxy (the port would hold every pager's proxies alive, and a port cannot know which owner type it holds).

**Reference**
Zircon `PagerProxy` (`object/pager_proxy.cc`): `SendAsyncRequest()`/`QueuePacketLocked()`, `ClearAsyncRequest()` with `CancelQueued()`, `OnDetach()` queuing `complete_request_`, `OnClose()`, `Free()` and `OnPacketFreedLocked()`, `WaitOnEvent()` with `userpager_overtime_wait_seconds` (20) and `userpager_overtime_timeout_seconds` (300). Not taken: `SwapAsyncRequest()` (a non-goal), `DIRTY` requests, `Dump()`. The pager dispatcher's list and `OnDispatcherClose()` come in 9b.

**Verification**
- `pager.test`, 10 checks, on a port, a proxy and a VMO over it, mapped into an address space of the test's own:
  - the VMO reports the proxy's owner id;
  - two faults waiting at once: a Read packet with the key, offset 0 and length one page, then, once it is read, a Read for page 1 in the same packet; a supply wakes both, and they read the supplied bytes;
  - two more faults, the first's packet left unread and the second's request in the proxy: a detach fails both, takes the Read back off the port, and Complete, with no range, is the only packet left;
  - with a 20 ms / 60 ms overtime, a fault nobody answers warns at 20 and 40 ms, gives up at 60 ms, and its packet is taken back off the port;
  - lifetimes, by weak references to the proxy and the source: the VMO gone with Complete unread keeps both until the pager reads it, then both go; the port closed with Complete still on it drops it, and both go; the port closed first, both go with the VMO.
- Control runs: without the cut in `free()`, the first two lifetime checks fail (the cycle leaks); with a refused Complete left pending, as Zircon has it, the port-first check fails; without `cancel_queued()` in `clear_async_request()`, the detach and overtime checks fail (a stale Read is left for the pager). Restored, all pass.
- `port.test`, `pagesource.test` pass. clang-tidy reports nothing on the files changed.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.

**Deliberately not done**
- The PagerDispatcher, its list of proxies, the pager closing first, and `VmoDispatcher::pager_koid()`: 9b.
- A packet for a VMO whose making failed after its source was made: closing that source sends Complete for a VMO the pager never saw (DESIGN.md's open question).
- Boot options for the overtime: constants, as nothing else here reads boot options for mm yet.

**How it was found**
None.

**The user's prediction**
None (homework paused since 2026-10-21).

**Three questions you should be able to answer**
- Why can the proxy not be destroyed while its packet is on the port, and what keeps it alive?
- Name the two places the cycle is cut in 9a, and what has to be true at each.
- What would happen to a Complete queued behind a Read that a closed port refused, if the refused packet were treated as busy?
