# kernel-irq changes

## Title: Callers own their interrupt observers, and a handler gets its context (2026-10-04)

**Files**
- `kernel/include/ours/irq/irq_observer.hpp` — the observer is built by its owner from a handler, a context, flags and a name; it records the line it hangs on; it can be neither copied nor moved.
- `kernel/include/ours/irq/types.hpp` — `IrqHandler`'s parameters are named, and the second one is the context.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `irq::attach(VIrqNum, IrqObserver &)` and `irq::detach(IrqObserver &)` replace `request_irq` and `release_irq`.
- `kernel/include/ours/irq/irq_dispatcher.hpp`, `kernel/kernel/irq/irq_dispatcher.cpp` — the same at the dispatcher, which no longer allocates anything.
- `kernel/kernel/irq/irq_object.cpp` — `attach` refuses a null handler and an observer already on a line, and records the line; `detach` checks that the observer is on this line.
- `kernel/arch/x86/kernel/time/time.cpp` — the device the legacy timer line drives lives in a `LegacyTimer` object, handed to the handler as its context; the line's observer is a static; the global `CLOCK_EVENT` goes.
- `kernel/arch/x86/kernel/include/ours/arch/time.hpp` — `extern CLOCK_EVENT` gives way to `global_clock_event()` and `set_global_clock_event()`.
- `kernel/arch/x86/kernel/time/hpet.cpp` — the hand-over to the HPET calls `set_global_clock_event()`.
- `kernel/platform/pc/keyboard.cpp` — the decoder's shift keys, last scancode and character buffer move into a `KeyboardState` handed to the handler as its context; the line's observer is a static.

**Symptom**
Latent, cannot be triggered today. Three defects of the interface, numbered 4 and 5 in `.agents/tasks/microkernel-interrupts/DESIGN.md`:
- Every `request_irq` that the line refused with AlreadyBound leaked the observer it had just allocated. Nothing registers twice on one line today, so nothing has leaked yet; the user-space `interrupt_create` of PLAN step 11 would have leaked one observer per refused attempt, a leak any process holding an IRQ resource could drive.
- `release_irq(virq)` answered Unimplemented, so nothing could ever come off a line.
- A handler's context argument was always `nullptr`, so both handlers read globals: the PIT's `CLOCK_EVENT` and the keyboard's `KEY_BUF`, `key_lshift`, `key_rshift` and `LAST_CODE`.

**Cause**
The question underneath all three is who owns the registration record, the observer that sits in the line's list.

`request_irq` answered "the irq layer": it allocated the `IrqObserver` with `new (mm::GAF_KERNEL)` and handed it to `IrqObject::attach`. When `attach` answered AlreadyBound, `request_irq` returned that status and dropped the only pointer to the allocation. The list did not hold it, and the caller never knew it existed. Linux's `request_threaded_irq()` also allocates its `struct irqaction`, but it frees it on every failure path: `kfree(action)` right after `__setup_irq()` fails. Ours was one `kfree` short. That `kfree` is not the whole fix, though. A layer that allocates the record cannot know how long the caller needs it to live, and in the design that comes next, the interrupt object of PLAN step 11 needs exactly that: its registration must end when the object ends, and not a moment later.

Releasing by line number had the same root. A line can hold several observers when it is shared, and the number cannot say which of them to take off. Linux's `free_irq(irq, dev_id)` needs the second argument for exactly this: `__free_irq()` walks the line's actions looking for the one whose `dev_id` matches. Naming the observer itself is the direct form of that search.

The missing context was what forced globals on the two handlers. A handler with no argument of its own can find its state only through something every handler can reach: a global, or a table indexed by line number. For the kernel's two drivers a global was tolerable. For an interrupt object, which must find *itself* from inside the handler, it is not; Zircon passes the object as the cookie, `interrupt_handler_t{this, ...}` in `InterruptEventDispatcher::RegisterInterruptHandler()`.

Making the caller the owner answers all three at once:
- The observer's lifetime is its owner's. A kernel driver keeps its observer in a static, alive as long as the kernel; an interrupt object will embed its observer, so the observer dies exactly when the object does, after the object has detached it.
- Nothing is allocated, so `attach` cannot fail for lack of memory and has nothing to free when it fails for any other reason.
- `detach` is given the observer, which knows its line, so on a shared line it removes exactly one observer.
- The observer carries the context its owner chose, and the line hands it to the handler on every interrupt.

Ownership by the caller brings one new way to go wrong: the owner might destroy an observer that is still linked into a line's list, which leaves the line walking freed memory at the next interrupt. The observer's list hook is ustl's default SafeLink kind, whose destructor executes `__builtin_trap()` if it is still linked, so that mistake stops the kernel at the destruction rather than corrupting it later. Copying and moving are deleted for the same reason: a copy of a linked observer would be a second, unlinked object with the same handler and context.

The observer records which line it hangs on (`m_virqnum`), so `detach` needs nothing else. `IrqObject` is its friend, and only `IrqObject::attach` and `detach` write that field, in the same breath as they link and unlink the hook, so the two cannot disagree.

**Change**
The dispatcher, before and after:

```cpp
// before
auto observer = new (mm::GAF_KERNEL) IrqObserver(irqnum, flags, handler, name);
if (!observer) {
    return Status::OutOfMem;
}
return object->attach(*observer);   // AlreadyBound: `observer` leaks

// after
return object->attach(observer);    // the caller's observer; nothing to free
```

The PIT, before and after:

```cpp
// before
ClockEventDevice *CLOCK_EVENT;
auto timer_interrupt(VIrqNum irqnum, void *) -> irq::IrqReturn {
    CLOCK_EVENT->handle_event();
    ...
}
auto status = irq::request_irq(ISA_IRQ_PIT, timer_interrupt, irq::IrqFlags::Permanent, "IRQ-Timer");

// after
constinit LegacyTimer LEGACY_TIMER;   // holds std::atomic<ClockEventDevice *>
auto timer_interrupt(VIrqNum, void *context) -> irq::IrqReturn {
    auto *const device = static_cast<LegacyTimer const *>(context)->device();
    device->handle_event();
    ...
}
constinit irq::IrqObserver LEGACY_TIMER_OBSERVER{&timer_interrupt, &LEGACY_TIMER,
                                                 irq::IrqFlags::Permanent, "IRQ-Timer"};
auto const status = irq::attach(ISA_IRQ_PIT, LEGACY_TIMER_OBSERVER);
```

The keyboard follows the same shape: `KeyboardState KEYBOARD` holds the shift keys, the last scancode and the buffer; `KEYBOARD_OBSERVER` carries `&KEYBOARD`; `i8042_interrupt` casts its context back and passes the state to `i8042_process_scode`. The two shift `bool`s became a `ShiftKeys` flags word, as the style rules ask of a set of yes/no facts about one object; both bits are written only by the decoder.

The trade-offs:
- **What the PIT's context is.** The device the legacy line drives changes once, when the HPET takes IRQ0 over through its legacy replacement route. So the context cannot be the PIT's device itself: re-pointing it would mean detaching and attaching again, which a Permanent line will refuse once PLAN step 6 enforces Permanent, as Zircon's `InterruptTableEntry::set_handler()` refuses any change to a permanent entry. The context is a small object holding the current device, and the HPET's hand-over goes through `set_global_clock_event()`, Linux's name for the same pointer.
- **Why that pointer became atomic.** `CLOCK_EVENT` was a plain pointer written in thread context and read by the interrupt handler, a data race in C++'s terms. The new object stores it with release and loads it with acquire, so a handler that sees the HPET's device also sees the writes that enrolled it. On x86 both compile to a plain `mov`.
- **The observer beside its state, or inside it.** The plan keeps each driver's observer as a static whose context is the driver's state, and that is what was done. Embedding the observer in the state object, context `this`, is the shape PLAN step 11's interrupt object takes.
- **Rejected: keep `request_irq` and add a context parameter.** It would still allocate, so it would need a `kfree` on every failure path and a way to find the allocation again at release, which is the bookkeeping the caller-owned form removes.
- **An invalid line number is no longer a panic.** `irq::request_irq` began with `ASSERT(virqnum != INVALID_V_IRQ_NUM)`; `irq::attach` answers InvArgs, as the dispatcher already did. Step 11 passes numbers that came from user space, and a caller's bad argument must not stop the kernel.
- **The new errors.** `attach` answers BadState for an observer already on a line, since its one hook cannot be in two lists, and InvArgs for a null handler, the check `request_irq` made. `detach` answers NotFound for an observer on no line or on another one. Zircon's unregister of an empty entry answers OK and Linux's `free_irq()` WARNs; a status the caller must look at sits between the two.

**Reference**
- Zircon `interrupt_handler_t {cookie, fn}` (`zircon/kernel/dev/interrupt/include/dev/interrupt.h:24-31`); `InterruptEventDispatcher::RegisterInterruptHandler()`, which passes `this` as the cookie, and `UnregisterInterruptHandler()` (`zircon/kernel/object/interrupt_event_dispatcher.cc:155-165`); `InterruptTableEntry::set_handler()`, which refuses to change a permanent entry (`zircon/kernel/platform/pc/interrupt_manager.rs:120-139`). Zircon's own PIT passes a null cookie and reads globals (`register_permanent_int_handler(irq, interrupt_handler_t{nullptr, &pit_timer_tick})`, `zircon/kernel/platform/pc/timer.cc:688-690`); ours passes its state, as the plan asks.
- Linux `request_threaded_irq()`, which allocates the `irqaction` and frees it when `__setup_irq()` fails (`kernel/irq/manage.c:2123-2190`); `free_irq(irq, dev_id)` and `__free_irq()`, which find the action by `dev_id` (`:2006`, `:1859`); `timer_interrupt()` and `global_clock_event` (`arch/x86/kernel/time.c:37-41`); `hpet_legacy_clockevent_register()`, which points `global_clock_event` at the HPET (`arch/x86/kernel/hpet.c:467`).

**Verification**
- Build of `out/bringup.x86`: ok.
- `boot_check.py`: dbg PASS (shell finished at 28.4 s, 24 passed, 1 skipped, 0 failed, 0 error lines); rel PASS (3.5 s, 22 passed, 3 skipped, 0 failed, 0 error lines). Reaching the shell needs the timer tick, first the PIT's through the new context and then the HPET's after `set_global_clock_event()`.
- The keyboard, by script rather than by hand: a script in the run's scratchpad, built on `scripts/kshell_run.py`'s `Console`, booted dev.dbg_1 and, after `--- end ---`, typed Ctrl-A c, `sendkey a`, `sendkey shift-b`, `sendkey c`, Ctrl-A c into QEMU's multiplexed monitor. The serial log shows `char 0x61 (a)`, `char 0x42 (B)` and `char 0x63 (c)`: the handler reached its state through the context, and the shift bit was set and cleared again.
- clang-format: clean on the changed lines. clang-tidy: nothing new on the changed lines. The four UPPER_CASE namespace-scope objects carry `NOLINTNEXTLINE(readability-identifier-naming)` with the reason, as `NOOP_DEVICE` in `kernel/kernel/time/tick_device.cpp` does.
- Not tested: `detach`, which nothing calls until PLAN step 6 makes it a barrier and adds `irq.test`. The irq layer has no host test target.

**Deliberately not done**
- `detach` is not a barrier: the line has no lock yet, so a handler in flight on another CPU may outlive the call. PLAN step 6 adds the lock; until then nothing calls `detach`.
- `Permanent` is still read by nobody, and `attach` still overwrites the line's stored flags (PLAN step 6).
- The PIT still registers by ISA number and the keyboard by GSI (PLAN step 7), and the PIT still unmasks its line after a failed attach (defect 9, PLAN steps 6 and 8).
- The keyboard's init still drains the controller by calling `i8042_interrupt(0, &KEYBOARD)` in thread context after unmasking the line, so an interrupt in the middle of it would run the decoder on the same state re-entrantly. That predates this change, and the file goes in PLAN step 12.
- `IrqObject`'s members keep their trailing-underscore names; PLAN step 6 rewrites the class.
- The HPET's hand-over is published after the HPET is already driving the line; recorded as `docs/repairs/261004/x86-clock.md` item 1.

**How it was found**
None. PLAN step 5 of `.agents/tasks/microkernel-interrupts/`, from defects 4 and 5 of its design.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does an irq layer that allocates its own observers leak on AlreadyBound, and why is adding the missing `kfree` not enough once interrupt objects can register?
- Why can the PIT's context not simply be the PIT's `ClockEventDevice`, and what would re-pointing the line by detach and attach run into once Permanent is enforced?
- An interrupt object that embeds its observer is destroyed without detaching it: what happens with a SafeLink hook, and what would have happened with a plain one?
