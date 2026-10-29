# loop changes

## Title: a Loop slot can wait on a Waiter, an abstract class, beside the callback function it has always had (2026-10-29)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — `class Waiter`, with the pure virtual `on_signals(observed) -> Action`, a protected, non-virtual, defaulted destructor and a `constexpr` constructor; the overload `Loop::add(handle, signals, Waiter &)`; `Callback`'s comment stops saying "never a virtual call", points at `Waiter` for the rules and says what the detour is; the comments of the namespace and of `Slot::m_callback` say what a Waiter registration stores
- `system/lib/loop/src/loop.cpp` — `call_waiter()` in the anonymous namespace, and the overload, which is `add(handle, signals, &call_waiter, &waiter)`
- `system/lib/loop/test/loop_test.cpp` — `WaiterClient`, a `Waiter` that does what the `Client` inside it does; `LoopTest::add_waiter()`; four tests of `add()` and seven of `run()`
- `system/lib/loop/README.md` — names `Waiter` and the detour

**Symptom**
Latent, cannot be triggered today. Nothing registers a `Waiter` yet, and `Slot`'s layout and `run()` are as they were. This is step 1 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. The user chose on 2026-10-29, after `docs/perf/261028/loop-dispatch.md`, that the Loop's callback, a function pointer and a `void *context`, becomes an abstract class like `ioservice::LoopServer::Delegate` and `launch::LauncherOps` before it (`docs/changes/261028/ioservice.md`, `launch.md`). Each of the six registration sites writes a static function whose only work is to cast the `void *` back to its own object, and that is what the new form removes.

**Cause**
Why the new form arrives beside the old one and not in its place. The six sites are spread over `ioservice`, the root's servers and `Realm`, and the echo example. Moving all of them in one commit would make a mistake in one of them hide among the others. So each step moves one site, and every commit must build and boot, which needs both forms to exist at once until the last site has moved.

Why it can share the slot. A slot already holds a function `Callback m_callback` and a `void *m_context`, and `run()` calls the one with the other. A Waiter is exactly a context that needs one fixed function: the function casts the context back to `Waiter *` and calls `on_signals()`. So `add(handle, signals, Waiter &)` stores `&call_waiter` and `&waiter`, and nothing else in the Loop knows a Waiter exists. That is what keeps `run()` untouched, and with it the invariants of its comment (I1, I3, I4): `run()` is the function the user reserved to write by hand, and a step that only adds a registration should not have to re-argue it.

Why `Waiter` is `loop::Waiter` and not `Loop::Waiter`, as the measurement report wrote it. `Slot` is declared before `Loop` and will hold the pointer in step 6, so `Waiter` has to be declared first, at namespace scope.

Why the destructor is protected and not virtual, and the constructor `constexpr`. Nothing is deleted through a `Waiter`, and a virtual destructor would add a deleting destructor, which calls `operator delete` in a user program that has no heap (the reason of `Delegate`, `docs/changes/261028/ioservice.md`). The constructor is `constexpr` as `LauncherOps`'s is: steps 2 and 3 make `LoopServer::Binding` and the root's connection pools `Waiter`s, and those are `constinit`, so a derived class must be constant-initialisable. The copy operations are protected and defaulted, as both siblings have them, so that a derived class is still copyable where its owner needs it.

Why the lifetime rule is written in `Waiter`'s comment. The old form had no such rule to state: a function and a pointer to something the caller keeps. A `Waiter` is an object, and the Loop keeps its address without owning it, so the comment says what the caller owes: it stays where it is from `add()` until the slot is freed, by its `Close`, by a failed re-arm, or by the Loop's destructor. After that no packet reaches it, because step 1 of `run()` drops a packet whose key names a slot not in use in its life.

**Change**
```cpp
// loop.hpp
class Waiter {
  public:
    virtual auto on_signals(ours::Signals observed) -> Action = 0;
  protected:
    constexpr Waiter() = default;
    constexpr Waiter(Waiter const &) = default;
    constexpr auto operator=(Waiter const &) -> Waiter & = default;
    constexpr ~Waiter() = default;
};

[[nodiscard]]
auto add(ours::Uandle handle, ours::Signals signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status>;

// loop.cpp
auto call_waiter(void *const context, ours::Signals const observed) -> Action {
    return static_cast<Waiter *>(context)->on_signals(observed);
}

auto Loop::add(ours::Uandle const handle, ours::Signals const signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status> {
    return add(handle, signals, &call_waiter, &waiter);
}
```
The overload has the old `add()`'s errors, less one: a reference cannot be null, so the remaining `InvArgs` causes are an `Invalid` handle and `Signals::None`. `NoResource` and `object_wait_async`'s answers come from the old `add()` unchanged, as does the clearing of the slot when the wait cannot be armed. The overload makes no system call and allocates nothing, so I1 of `run()` holds.

The cost, plainly: **until step 6 a call to a Waiter costs one more indirect branch than a call to a function does.** `run()` calls `slot.m_callback(...)`, an indirect call to `call_waiter`, and `call_waiter` jumps through the object's vtable. On the host (arm64, `llvm-objdump` of the object file) `call_waiter` is three instructions, `ldr x8, [x0]`, `ldr x2, [x8]`, `br x2`: it loads the vtable pointer and the function, and tail-jumps, with no frame of its own. The first branch always lands on the same function, so the predictor is right every time; the second is the virtual call, whose target varies with the slot, and is the one that `docs/perf/261028/loop-dispatch.md` measured (B against A: 0.76 ns more, and only when the target is mispredicted). So the detour adds the predictable branch and the two loads to what that report measured; it is not measured by itself. It is accepted because it lasts five steps and goes in step 6, when `Slot` holds the `Waiter *` and `run()` calls `on_signals()` directly, which is the form the report measured; because the virtual call it measured, 0.76 ns at the most, is small beside the 580 ns of one `write(2)` on the same machine; and because in this commit nobody registers a Waiter outside the tests, so the images run exactly as before. Rejected: giving `Slot` a second pointer now, or a union of the two forms, which would change the slot and `run()` in a step that exists to leave them alone, and would be taken out again in step 6 in any case.

**Reference**
None, this project's own design. The form of `Waiter` follows the two abstract classes this project made on 2026-10-28, `ioservice::LoopServer::Delegate` and `launch::LauncherOps`.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests`, then `out/bringup.x86/host/obj/system/lib/loop/test/loop`: 29 passed, the 18 tests of before and 11 new ones. The new ones: `AddWithAWaiterTakesTheLowestFreeSlotAndArmsOneWaitUnderItsKey`, `...RefusesAnInvalidHandleOrNoSignals`, `...AnswersNoResourceWhenEverySlotIsInUse` (the table is filled with both forms alternately), `...LeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails`, and seven `Run*` tests: the Waiter whose wait fired is called with the observed signals, `PeerClosed` is passed on as it came, `Rearm` re-arms under the same key and signals, `Close` closes the handle once and frees the slot, a Waiter and a function share one Loop and are each called for their own slot only, only the slot that answers `Close` is closed, and a Waiter may `add()` from inside `on_signals()`.
- Three deliberate breakages of `call_waiter()`, the source put back after each (the final run is again 29 passed). It answers `Close` whatever `on_signals()` said: 2 tests fail, `RunRearmsAWaiterUnderTheSameKeyAndSignals` and `RunCallsAWaiterAndAFunctionEachForItsOwnSlot`. It passes `Signals::None` as `observed`: 4 fail, `RunCallsTheWaiterOfTheSlotWhoseWaitFiredWithTheObservedSignals`, `RunTellsAWaiterWhatTheObjectAsserted`, `RunRearmsAWaiterUnderTheSameKeyAndSignals`, `RunCallsAWaiterAndAFunctionEachForItsOwnSlot`. It answers `Rearm` whatever `on_signals()` said: the test binary did not end and had to be killed. That is the spin that `Waiter`'s comment warns of: the Waiter that answered `Close` with its message unread is re-armed, its level-triggered wait queues its packet again at once, and `run(0)` never finds the port empty. It is a catch, but as a hang and not a failure; the tests of the function form would hang the same way under the same change to `run()` (not tried).
- `prebuilt/ninja/ninja -C out/bringup.x86` (1043 steps, which builds init, echo, the shell, `ioservice` and its tests) and `phony/benches/benches` build; `loop-dispatch-bench check` prints `check ok`.
- `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86 --header-filter='.*/system/lib/loop/.*'` on `loop.cpp` and `loop_test.cpp`: no diagnostic. `format_cpp.py --check` on the three C++ files: clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-api` in `.worktrees/lw-api` with the final tree: build ok in 7.6 s; dbg PASS, shell finished at 5.6 s, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, shell finished at 4.9 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines; host tests PASS, 148 passed, 0 failed. `rel.log` holds `routing_checks: passed` and `shell: loop checks passed`, and `dbg.log` the second. No program registers a Waiter, so the images behave as before; this shows that adding the class and the overload broke none of the six sites.
- Not measured: the cost of the detour on its own, and anything on the kernel's x86-64 hardware.

**Deliberately not done**
- No registration site moves: `LoopServer`, `Outgoing`, the root's servers, `Realm`, echo and the shell's `check_loop()` still use the function form. They are steps 2 to 5.
- `Slot` and `run()` are not touched; neither is `Callback` removed or marked, which is step 6.
- No virtual destructor, no `override` of anything but `on_signals()`, no RTTI.
- `Waiter`'s copy is not deleted; see the Cause.
- No check that a `Waiter` is not registered twice: two slots may share one `Waiter`, and `on_signals()` can tell them apart only by what it observes. Nothing needs more than that today.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `add(handle, signals, Waiter &)` calls the old `add()` with `&call_waiter` and `&waiter` instead of storing a `Waiter *` in the slot. What would storing it need to change, which step does that, and what does the detour cost in the meantime?
2. The context travels as a `void *` and is cast back with `static_cast<Waiter *>`. Why is that correct for a class with several bases, and what would go wrong if the registration stored the address of a derived object instead of the address of its `Waiter`?
3. A Waiter answers `Rearm` with its message still unread. What does `run(0)` do, why can the tests not turn it into a failing assertion, and which sentence of the spec covers it?
