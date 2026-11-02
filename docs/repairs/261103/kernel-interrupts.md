# kernel-interrupts outstanding repairs

## 1. An interrupt cannot join a loop::Loop: no path delivers it to a port the Loop waits on

**Severity**
Medium: wrong capability, not wrong results — every interrupt-driven user-space server is blocked on it, and the first one (NVMe) is exactly the kind this kernel wants. The block-driver library's async path is complete and proven by host tests; it has no interrupt to complete from.

**Symptom**
Latent, cannot be triggered today: virtio-blk completes inside its `start()` hook by polling `interrupt_wait` inline, as it always did, so everything boots and the disk checks pass. The gap shows when a driver author tries the async path the library documents: there is no call sequence that runs a completion callback on the Loop thread when the line fires.

**Cause**
Three dead ends, each a wall:
1. `object_wait_async` on an interrupt handle is not supported — the interrupt dispatcher parks waiters on an internal `sync::Event` (`kernel/object/interrupt_dispatcher.cpp`), it is not a signal object, so the syscall answers BadType.
2. `interrupt_bind` delivers only to a port created `PortCreateOptions::BindToInterrupt`, and the Loop's port is created with `None` — the caller cannot pass the Loop its interrupt packets.
3. `interrupt_wait` blocks the calling thread, which a Waiter's `on_signals()` must never do (the Loop's rule I1: the one thread blocks only in port_wait).

The Loop drops any packet whose key does not name one of its slots, so an interrupt bound to a foreign port-key is dropped at step 1 of run()'s dispatch. The fake kernel models none of this (it has no interrupts), so the gap is invisible to the host suites.

**Change**
Two candidate shapes, to be decided with the kernel side:
- **Wait-async on interrupts** (the Zircon shape): the interrupt dispatcher gains the signal-observer machinery — `object_wait_async(interrupt, port, key, Readable)` arms one one-shot or recurring wait, delivered to any port. The Loop then sees an interrupt like any handle: `loop.add(interrupt, Readable, waiter)`, one slot, the Waiter drains the used ring in `on_signals()` and answers Rearm (for the recurring shape) or re-adds (for the one-shot).
- **A Loop-created BindToInterrupt port**: the Loop takes an optional second port created `BindToInterrupt`, and `interrupt_bind` delivers into it; run() multiplexes two ports or the packets cross onto the main port. More moving parts, and it invents a second key space.

The first is the Zircon counterpart (`zx_interrupt_bind` delivers to any port created with `ZX_PORT_BIND_TO_INTERRUPT`, and `zx_object_wait_async` is not supported on interrupts either — the Loop-side gap is ours alone); the second is this project's own. Either way the block-driver library needs no change: `complete()` is callable from any Loop callback, so the driver's IRQ waiter just calls it.

**Reference**
`zx_interrupt_bind` requires `ZX_PORT_BIND_TO_INTERRUPT` (`docs/reference/syscalls/interrupt_bind.md` in the Fuchsia source tree), `zx_object_wait_async` is documented as unsupported on interrupt objects; Zircon drivers block a dedicated thread in `zx_interrupt_wait` — a shape ours could adopt per-driver, at the cost of a thread per device.

**Acceptance**
A driver (virtio-blk as the pilot) parks a request via the block library, binds its line to the Loop by one of the two shapes, and its completion runs `complete()` from `on_signals()`; the disked routing_checks pass with completions arriving from the Loop instead of the inline poll. Until then the async path's proof stays the host suite.
