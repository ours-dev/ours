# loop

One port and a fixed table of wait slots, through which a single-threaded server waits on many handles at once: the root's every wait (`docs/design/261004/component-framework.md`, "Concurrency: one thread and one Loop"), and a provider's, through `ioservice::Outgoing` (`examples/echo`). The counterpart of Fuchsia's async-loop (`sdk/lib/async-loop/loop.c`), cut down to waits.

- `loop/loop.hpp`: `Loop`, `Slot`, `Key`, `Action` and `Waiter`. `add(handle, signals, waiter)` takes the lowest free slot and arms a one-shot wait whose key is `generation << 32 | index`; `run()` takes packets, calls the `on_signals()` of each slot's `Waiter` once, and re-arms the slot, closes it, or suspends it as the `Waiter` answers (`Rearm`, `Close` or `Suspend`); `resume(key)` arms a suspended slot's wait again (below).
- A slot waits on a `Waiter`, an abstract class whose `on_signals()` `run()` calls. The caller owns the `Waiter`: it stays where it is from `add()` until the slot is freed. A `Waiter` has a second virtual function, `on_rearm_failed(cause)`, which `run()` calls when the `Waiter` answered `Rearm` and the Loop could not arm the wait again: the handle is closed and the slot freed by then, and the `Waiter`'s owner releases what it kept for the slot. It does nothing by default, which is right for a `Waiter` that only answers `Close` (`docs/changes/261029/loop.md`). A slot holds one pointer to it, so a `Slot` is 24 bytes where it was 32 while a slot held a function and a `void *` context (`loop::Callback`, removed on 2026-10-29: `docs/changes/261029/loop.md`). It is still 24 with the flag that marks a suspended slot: {handle 4, signals 4, waiter 8, generation 4, flags 1}, the flag in what was 4 bytes of padding (`test/loop_test.cpp` asserts the size).
- `run()` is the function P3 in `.agents/tasks/component-framework/PLAN.md` reserves for the user to write by hand. Its comment in `loop.hpp` is the whole spec. For the MVP of 2026-10-04 the AI wrote it on the user's instruction; the hand-writing exercise stays open.

## Suspend and resume

A `Waiter` answers `Suspend` when it has taken a unit of work it cannot finish yet and must not be handed the next one before it has: a connection that took a request whose reply waits on another party. `run()` marks the slot suspended and does nothing else. No wait is armed, the handle stays open, and the generation is not bumped, so the slot stays in use (`add()` never takes it) and its key stays valid. What arrives on the handle meanwhile stays in the object: a channel keeps its messages and its closed peer, and costs the Loop nothing, where a `Rearm` over an unread message would spin. `Loop::resume(key)` arms the wait again, under the same key and signals. The wait is level-triggered, so a message that arrived, or a peer that went, queues its packet at once, behind the packets already queued; a peer that closed is then seen as `PEER_CLOSED` alone, and the `Waiter` answers `Close` for it as for any closed peer. A `Waiter` that answers `Suspend` and is never resumed keeps its slot and its handle for good; the destructor of the `Loop` closes a suspended slot's handle like any other.

`resume(key)` answers by the state of the slot the key names, and changes nothing unless it answers `Ok` or the arm is refused:

| The slot | `resume` | Effect |
|---|---|---|
| in use, suspended, generation current | `Ok` | the flag is cleared and the wait armed |
| the same, and the kernel refuses the arm | the refusal | as a failed re-arm in `run()`: the handle is closed, the generation bumped, the slot freed, and the `Waiter`'s `on_rearm_failed(cause)` is called once before `resume` returns |
| in use but not suspended: armed, its packet queued (so also a second `resume`), or its own `Waiter` running | `BadState` | none |
| free, a generation other than the key's, or an index past the table | `NotFound` | none: the key is stale |

A `resume` called inside the `on_signals()` that is about to answer `Suspend` is `BadState`, because `run()` marks the slot only after `on_signals()` returns. So whoever resumes must be called later, from another slot's `Waiter` or from between runs, never from the call that suspends.

A connection that takes a request, suspends, and is resumed when its answer is ready (a sketch; `Lookup` stands for any other slot's work):

```cpp
struct Connection final: loop::Waiter {
    auto on_signals(ours::Signals const observed) -> loop::Action override {
        if ((observed & ours::Signals::Readable) == ours::Signals::None) {
            return loop::Action::Close; // PEER_CLOSED alone: the client is gone
        }
        read_request();                 // one unit of work
        m_lookup->start(*this);         // the answer comes later, from another slot
        return loop::Action::Suspend;   // do not read the next request yet
    }

    // Called by the Waiter of the slot the lookup waits on, when it is done.
    auto answer_ready() -> void {
        write_reply();                  // the reply first, so that the next
                                        // request never starts before this
                                        // one is answered
        auto _ = m_loop->resume(m_key); // the key add() returned
    }

    loop::Loop *m_loop;
    loop::Key m_key;
    Lookup *m_lookup;
};
```

The objects are the connection's channel, its slot `S` and key, and the lookup's slot `L`. The events: the client writes `Request 1` and a second request; `S` fires and `on_signals()` reads the first, starts the lookup and answers `Suspend`; the second request waits in the channel while `S` is not armed; the lookup's answer arrives, `L` fires, and its `Waiter` calls `answer_ready()`, which writes the reply and resumes `S`; the channel holds the second request, so `S`'s packet is queued at once and reaches `on_signals()` in its turn, behind everyone else's.

## Layout

```text
include/loop/   public headers, reached as <loop/...>
src/            sources
test/           host-side tests, built with googletest, and the fake kernel
```

## Building and testing

The library is `//system/lib/loop`; its headers alone are `//system/lib/loop:headers`. It calls the vDSO's `port_wait`, `object_wait_async` and `handle_close` and nothing else. On the target the vDSO binds those calls; on the host, `test/fake_kernel.cpp` defines the same three symbols, so the test runs the library's own source.

```text
prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests
out/bringup.x86/host/obj/system/lib/loop/test/loop
```

The shell's `check_loop()` (`system/app/shell/shell_tests.cpp`) runs the Loop on real channels and a real port, on both images, and prints `shell: loop checks passed`. It includes a slot that suspends: not called while its channel holds a message, called after `resume`, and answered `BadState` by a second `resume`.
