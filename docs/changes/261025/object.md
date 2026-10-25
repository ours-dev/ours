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
