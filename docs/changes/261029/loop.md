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

## Title: A slot waits on a Waiter, and the function-pointer form and the detour that carried the Waiter over it are gone (2026-10-29)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — `Slot` holds `Waiter *m_waiter` in place of `m_callback` and `m_context`; `Callback` and the `add(handle, signals, Callback, void *)` overload are deleted, and `add(handle, signals, Waiter &)` is the one `add()`; the comments of the namespace, `Waiter`, `Slot`, `add()` and `run()` (steps 2 to 4, I3, I4, the generation paragraph) say the slot's Waiter and `on_signals()` where they said the callback and the context; "and later `ioservice::Outgoing`'s" is corrected
- `system/lib/loop/src/loop.cpp` — `call_waiter()` is deleted; `add()` does the checks and the work itself; `run()` calls `slot.m_waiter->on_signals(...)`; step 4 and `add()`'s failure path clear `m_waiter`
- `system/lib/loop/test/loop_test.cpp` — `Client` is the `Waiter`, and `serve()`, `WaiterClient` and `add_waiter()` are gone; the 18 tests of the function form and the 11 of step 1 are 21 tests; `static_assert(sizeof(loop::Slot) == 24)`
- `system/lib/loop/test/fake_kernel.hpp`, `system/lib/loop/README.md` — one comment, and the README's account of the slot
- `system/app/shell/shell_tests.cpp`, `shell_tests.hpp` — `check_loop()`'s `LoopClient` is a `loop::Waiter`, and `serve_one()` is its `on_signals()`
- `benches/system/lib/loop/loop_dispatch_bench.cpp` — a local `Callback` alias for form A, and the comments that said `loop::Callback` "is today"
- `docs/perf/261028/loop-dispatch.md` — Status Done, a Decision in the Conclusion, the gross saving corrected
- `docs/repairs/261029/loop.md` — deleted: its one item, the "later" in two comments, is fixed here
- `.agents/tasks/loop-waiter-remove/PLAN.md`, `RESULT.md` — this step's workspace

**Symptom**
Not a defect: the Loop behaved correctly at the end of step 5. This is step 6 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. After steps 2 to 5 nothing in `system/`, `examples/` or `kernel/` registered with the function-pointer form any more, but the Loop still carried it, and every Waiter was reached through it:
- a slot was 32 bytes, with a function and a `void *` of which the Waiter form used a fixed function and the Waiter;
- a call to a Waiter was two indirect branches, `run()` to `call_waiter()` and `call_waiter()` through the vtable (`docs/changes/261029/loop.md`, step 1, "The cost, plainly");
- every program with a Loop carried two `add()`s (259 and 244 bytes) and `call_waiter()` (10 bytes), where one is enough.

**Cause**
Why this is the moment. Step 1 put the Waiter beside the function so that each of the six registration sites could move alone and every commit boot. The last site moved in step 5, so the old form has no caller left but the Loop's own tests and the shell's `check_loop()`, and each of those moves in this step. From here the detour only costs: it keeps `Slot` at 32 bytes, it lets a new site write a trampoline again, and its `add()` has a null-function error that no caller can reach.

What changes in the slot, and what does not in the dispatch. `Slot` held `{handle, signals, callback, context, generation}`; it holds `{handle, signals, waiter, generation}`. The layout, from `-fdump-record-layouts-simple` on the compile command of `loop.cpp`, for the x86-64 user target and the arm64 host alike:

| | Before | After |
|---|---|---|
| `Slot` size, data size | 32, 28 | 24, 20 |
| field offsets | handle 0, signals 4, callback 8, context 16, generation 24 | handle 0, signals 4, waiter 8, generation 16 |

The four steps of `run()` are as they were, in the same order, with the same checks; the only differences are what step 2 calls (`slot.m_waiter->on_signals(observed)`, the virtual call, where it was `slot.m_callback(slot.m_context, observed)`) and what steps 4 and `add()`'s failure path clear. I1 holds because nothing was added: the host test links a fake of exactly `port_wait`, `object_wait_async` and `handle_close`, and `llvm-objdump` of `run()` shows three calls through the vDSO table (those three, `object_wait_async` through `arm()`) and the Waiter's virtual call, `callq *(%rax)`; nothing allocates. The generation check of step 1 (I3, I4) is untouched.

Why `m_waiter` is never null where `run()` dereferences it. `run()` reaches step 2 only for a slot whose handle is not Invalid (step 1), and a slot's handle and Waiter are set together and cleared together: `add()` stores the handle and `&waiter` after its argument checks and before it arms, and on a failed arm clears both; step 4 clears both; the constructor makes every slot with both empty. A reference cannot be null, so the old form's third `InvArgs` (a null callback) has no counterpart: the compiler refuses what the function checked at run time. `add()` now answers `InvArgs` for an Invalid handle or `Signals::None`, `NoResource`, and `object_wait_async`'s answers, which is the list step 1 gave for the Waiter overload.

Why a Waiter keeps its address stable is `Waiter`'s own comment, and steps 2 to 5 argued it for each site; this step adds nothing to it, except that the Loop now keeps the address itself in the slot, and not as a `void *` cast back by a function.

The account of the whole task's memory, measured. The brief asked for the net memory bill, so three trees were built into `out/bringup.x86` with the same toolchain: the one before the task (`75099335`), the one before this step (`75cb036b`, steps 1 to 5 in) and this step's. `llvm-size -A` and `llvm-objcopy --strip-all` on the two programs that have a Loop, `init` and `echo_server`:

| `init` | Before the task | Before this step | After this step | This step | Whole task |
|---|---|---|---|---|---|
| `.text` | 108,494 | 109,278 | 108,878 | -400 | +384 |
| `.data` | 28,928 | 51,360 | 51,360 | 0 | +22,432 |
| `.bss` | 77,664 | 56,704 | 55,296 | -1,408 | -22,368 |
| `.data` + `.bss` | 106,592 | 108,064 | 106,656 | -1,408 | +64 |
| `.data.rel.ro` | 2,168 | 2,296 | 2,296 | 0 | +128 |
| `.rela.dyn` | 4,992 | 5,712 | 5,712 | 0 | +720 |
| stripped file | 154,112 | 178,176 | 177,776 | -400 | +23,664 |

| `echo_server` | Before the task | Before this step | After this step | This step | Whole task |
|---|---|---|---|---|---|
| `.text` | 25,342 | 25,694 | 25,294 | -400 | -48 |
| `.data` | none | none | none | | |
| `.bss` | 22,960 | 23,024 | 22,960 | -64 | 0 |
| `.data.rel.ro` | 344 | 392 | 392 | 0 | +48 |
| `.rela.dyn` | 552 | 600 | 600 | 0 | +48 |
| stripped file | 30,192 | 30,640 | 30,240 | -400 | +48 |

Where the numbers come from. The slot tables are `SLOTS` in `init` (176 slots, `llvm-nm -S`: 5,632 bytes to 4,224) and `SLOT_STORAGE` in `echo_server` (8 slots: 256 to 192). The objects that became Waiters, from the sections of steps 2 to 5:

| Object | Before the task | After | Count | Vptrs paid |
|---|---|---|---|---|
| `LoopServer::Binding` | 16 | 24 | 128 in `init`'s `BootServer::Storage`, 4 in echo's `Outgoing` | 1,024 in `init`, 32 in echo |
| `ResourceConnection` | 16 | 24 | 20 in `init` (5 servers of 4) | 160 |
| `LauncherConnection` | 5,112 | 5,120 | 4 in `init` | 32 |
| echo `Client` | 16 | 24 | 4 in echo | 32 |
| `Instance` | 224 | 256 | 8 in `init` | 256 |
| `Slot` | 32 | 24 | 176 in `init`, 8 in echo | saves 1,408 and 64 |

The arithmetic closes against the sections. In `init`, 1,024 + 160 + 32 + 256 = 1,472 bytes of vptrs and back pointers were paid by steps 2 to 5, which is the growth of `.data` + `.bss` from 106,592 to 108,064; this step saves 176 x 8 = 1,408, which is the fall to 106,656; the task's net is +64 bytes. In echo, 32 + 32 = 64 bytes were paid and 8 x 8 = 64 saved: net 0, and `.bss` is the same 22,960 bytes. So the gross saving of the report, "8 KiB in a table of 1024 slots", is the figure of a table no program has; the two programs' tables save 1,408 and 64 bytes, and the objects that registered in them paid back 1,472 and 64.

Net per registered object, which is what the plan expected ("about zero"):
- A connection (`Binding`, `ResourceConnection`, `LauncherConnection`, echo `Client`) pays one vptr, 8 bytes, and its slot saves 8: **0**.
- An `Instance` pays 32 (two waiters of a vptr and a back pointer each, over a pool of 8) and its two slots save 16: **+16**, which is 128 bytes for the 8 instances.
- The table is sized for 176 slots and 168 of them have a Waiter that was made bigger (152 connections and 16 waits of instances); the 8 slots left save 64 bytes with nothing to pay back. `pools.hpp` counts three waits per component and `Realm` registers two Waiters per `Instance`; I did not trace the third. 128 - 64 is the +64.

What did not come back is the other cost: the five resource servers' storages moved from `.bss` into `.data` in step 3, and the stripped file is 23,664 bytes larger than before the task, of which 22,432 are the `.data` growth (the RW segment's file size, 0x7100 to 0xc8a0) and 1,232 are `.text`, `.data.rel.ro` and `.rela.dyn` (384 + 128 + 720). That is `docs/repairs/261029/init.md` item 1, not touched here.

Code size: `.text` falls by 400 bytes in each program, from the Loop alone. The object file of `loop.cpp`, compiled with the user target's command from the tree before and after (`llvm-nm -S`): the callback `add()` 259 bytes, the Waiter `add()` 244 and `call_waiter()` 10 become one `add()` of 219; the constructor 448 to 376 (it clears one pointer fewer), `~Loop()` 92 to 108, `run()` 331 to 313; the object's `.text` is 1,483 to 1,081 bytes, 402 fewer. The dispatch loses the second indirect branch: `run()` loads the slot's Waiter, its vtable and the function, and calls (`movq 0x8(%r13),%rdi; movq (%rdi),%rax; callq *(%rax)`), and a slot is now 3 words (`lea (%r12,%r12,2)` and a scale of 8).

The stale repair. `loop.hpp` and `README.md` said `ioservice::Outgoing` would use the Loop "later" when it already does (`docs/repairs/261029/loop.md` item 1). Both files were rewritten here, and both sentences now say what `Outgoing` is to the Loop. The repair file had no other item and is deleted.

**Change**
```cpp
// before: loop.hpp
using Callback = auto (*)(void *context, ours::Signals observed) -> Action;
class Slot final {
    Callback m_callback = nullptr;
    void *m_context = nullptr;
    // ...
};
[[nodiscard]]
auto add(ours::Uandle handle, ours::Signals signals, Callback callback, void *context)
    -> ustl::Result<Key, ours::Status>;
[[nodiscard]]
auto add(ours::Uandle handle, ours::Signals signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status>;

// before: loop.cpp
auto call_waiter(void *const context, ours::Signals const observed) -> Action {
    return static_cast<Waiter *>(context)->on_signals(observed);
}
auto Loop::add(..., Waiter &waiter) -> ustl::Result<Key, ours::Status> {
    return add(handle, signals, &call_waiter, &waiter);
}
// run(), step 2
auto const action = slot.m_callback(slot.m_context, packet.signal.observed);

// after: loop.hpp
class Slot final {
    Waiter *m_waiter = nullptr;
    // ...
};
[[nodiscard]]
auto add(ours::Uandle handle, ours::Signals signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status>;

// after: loop.cpp
auto Loop::add(ours::Uandle const handle, ours::Signals const signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status> {
    if (handle == ours::Uandle::Invalid || signals == ours::Signals::None) {
        return ustl::err(ours::Status::InvArgs);
    }
    // ... the lowest free slot, as before
    slot.m_handle = handle;
    slot.m_signals = signals;
    slot.m_waiter = &waiter;
    // ... arm; on failure the slot is cleared, m_waiter included
}
// run(), step 2
auto const action = slot.m_waiter->on_signals(packet.signal.observed);
```
The tests. `Client`, the test's per-slot object, is now `struct Client final: loop::Waiter` and its `on_signals()` is what `serve()` was. 18 tests of the function form and the 11 that step 1 added for the Waiter form are 21; the mapping is in `.agents/tasks/loop-waiter-remove/RESULT.md`. Two cases have no counterpart and went: the null callback refused (`AddRefusesAnInvalidHandleNoSignalsOrNoCallback` lost its third check), and the function-and-Waiter mix on one Loop (`RunCallsAWaiterAndAFunctionEachForItsOwnSlot`, and its sibling for closing, are now two Waiters). A `static_assert(sizeof(loop::Slot) == 24)` is in the test file and not in the header, so that the library does not assert the layout of the targets it is built for. The shell's `check_loop()` moved the same way: `LoopClient` has a constructor (a class with a virtual function is no aggregate), and `serve_one()` is its `on_signals()`.

The bench. Form A of `loop_dispatch_bench.cpp` mirrored the Loop's former callback with `loop::Callback`. The alias is now the bench's own, with a comment that says form A is the Loop's former form, kept as the baseline of the report. Nothing it measures changed, and the report's samples (`docs/perf/261028/loop-dispatch/*.csv`) are as they were; the bench was not run again for samples (`loop-dispatch-bench check` prints `check ok`).

Rejected: keeping `Callback` for a future caller that has no object to be a Waiter (a function of its own and a context); the project has none, and a caller that has one can write a small Waiter, which is what all six sites did. Putting the `static_assert` in the header; see above.

**Reference**
None, this project's own design. It finishes `.agents/tasks/loop-waiter/PLAN.md`, which follows the abstract classes of 2026-10-28 (`ioservice::LoopServer::Delegate`, `launch::LauncherOps`).

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build) and `phony/benches/benches` build with `-Werror`. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 21 passed (it was 29 with the 11 of step 1; see the mapping), `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 13, `host/obj/system/service/init/test/autorun` 5, all with 0 failed. `loop-dispatch-bench check` prints `check ok`.
- `grep -rn "loop::Callback\|Callback\b" system examples kernel benches --include='*.cpp' --include='*.hpp'` finds, besides three unrelated types of the kernel (`ccrt`'s `Callback` union, `gktl::Command::Callback`, a word in `dpc.hpp`), only the bench's own alias and the comments about it. Nothing names the removed alias. `grep` for `call_waiter`, `m_callback` and `m_context` in the Loop's files finds nothing.
- clang-tidy against `out/bringup.x86` with `--header-filter` on the library, the bench and the shell: `loop.cpp`, `loop_test.cpp` and `loop_dispatch_bench.cpp` exit 0 with no diagnostic. `shell_tests.cpp` exits 0 with 3 diagnostics, all `readability-identifier-naming` at lines 5030, 7257 and 7264, none of them in lines of this change (`H`, `sink`, `dump`). `scripts/format_cpp.py` on the seven C++ files: clean. `scripts/lint_docs.py --changed` on each document: no violations.
- Deliberate breakages in `Loop::run()`, the source put back after each (`cmp` against a saved copy), each shown on a host test and on the target's `check_loop()` (dbg image, `boot_check.py --only dbg --no-tests`):
  - step 2 passes `Signals::None` to `on_signals()`: 8 of the 21 loop tests fail (`RunCallsTheWaiterOfTheSlotWhoseWaitFired`, `RunRearmsUnderTheSameKeyAndSignals`, `RunServesABusyClientInTurns`, `RunClosesTheSlotWhenItsWaiterAnswersClose`, `RunClosesTheSlotWhenTheRearmFails`, `RunDropsPacketsThatNameNoSlotInUse`, `RunCallsEachWaiterForItsOwnSlotOnly`, `RunClosesOnlyTheSlotWhoseWaiterAnswersClose`); on the target `dbg FAIL`, `shell: loop served "ab" (expected "abaa")` and `shell: loop checks FAILED`.
  - step 3 skips the re-arm (`if (action == Action::Rearm) {` without `arm()`): 6 of 21 fail (`RunRearmsUnderTheSameKeyAndSignals`, `RunServesABusyClientInTurns`, `RunClosesTheSlotWhenTheRearmFails`, `RunDropsPacketsThatNameNoSlotInUse`, `RunCallsEachWaiterForItsOwnSlotOnly`, `RunClosesOnlyTheSlotWhoseWaiterAnswersClose`); on the target `dbg FAIL`, `shell: loop served "ab" (expected "abaa")` and `shell: loop checks FAILED`. Neither hangs: the target check dispatches with deadline 0.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-remove`, in `.worktrees/lw-remove` with the final tree: build ok in 6.6 s; dbg PASS (shell finished at 5.5 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.7 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `routing_checks: passed`, `init: echo exited with 0`, `pc-ps2: key a` and `pc-ps2: key A`; both logs hold `shell: loop checks passed`, which is `check_loop()` on a Waiter.
- Not measured: the time of one dispatch through the real `Loop::run()` before and after (the second indirect branch it saves is one the predictor always got right); the shell's own sections; the size of the BootFS image; anything on x86-64 hardware.

**Deliberately not done**
- No timing of the real `Loop::run()`. The user asked for none here; `.agents/tasks/loop-waiter-remove/RESULT.md` proposes one and says what it could show.
- No change to the dispatch rules, `Key`, `Action`, the slot table's sizing or the kernel.
- `docs/guide/` is not touched: it quotes `loop.hpp` and `loop.cpp` by line, and those lines moved. Another session owns it and will recheck it.
- The informal word "callback" in other libraries' comments (`ioservice`, `component`) stays: it says a function the Loop calls in the plain sense, as steps 2 and 3 left it.
- The five resource storages stay in `.data`; see `docs/repairs/261029/init.md`.
- No reordering of `Slot` to save the 4 bytes of padding: with an 8-byte pointer, 20 bytes of data round up to 24 at any order.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `run()` calls `slot.m_waiter->on_signals()` with no null check. Which invariant between the slot's handle and its Waiter makes that safe, which three places keep it, and what became of the `InvArgs` that a null callback used to get?
2. A `Slot` is 8 bytes smaller, yet over the whole task `init`'s `.data` + `.bss` grew by 64 bytes and its file by 23,664. Where do the 1,408 bytes this step saves go, which objects paid the 1,472 before, and what makes the file grow by so much more than the RAM?
3. This step changed no registration site, yet `.text` fell by 400 bytes in both programs. Which functions went, and why is the dispatch itself one indirect branch shorter?

## Title: the Loop tells a Waiter when it cannot arm its wait again, through a second virtual function, on_rearm_failed() (2026-10-30)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — `Waiter` gains `virtual auto on_rearm_failed(ours::Status cause) -> void`, with an empty inline body; its comment says when it is called, what state the Loop leaves the slot in, and when it is not called; `run()`'s steps 3 and 4 and its invariants I1 and I3 say the new call; the namespace comment says it
- `system/lib/loop/src/loop.cpp` — `run()` keeps the status of step 3's `arm()`, and step 4 calls `waiter.on_rearm_failed(rearm)` after the slot is free, for a failed re-arm only
- `system/lib/loop/test/loop_test.cpp` — `Client` records what it is told; six tests are new (`RunTellsTheWaiterWhenTheRearmFailsForWantOfMemory`, `...OnABadHandle`, `RunDoesNotTellAWaiterThatAnsweredClose`, `RunLetsTheWaiterAddFromOnRearmFailed`, `AddDoesNotTellTheWaiterWhenItFails`, `Loop.DoesNotCallOrTellItsWaitersWhenItGoes`), and `RunClosesTheSlotWhenTheRearmFails` expects the log `a!ab`
- `system/lib/loop/README.md` — names the second function
- `system/service/init/realm.hpp` — the comments of `OutgoingWaiter` and `TerminatedWaiter` say why they keep the default (see `init.md` of this day)
- the owners that override it are in `ioservice.md`, `init.md` and `echo.md` of this day

**Symptom**
Latent, cannot be triggered today; it needs `object_wait_async` to refuse the re-arm of a handle it accepted when the slot was added. If it did, `Loop::run()` closed the slot's handle and freed the slot, and the `Waiter`'s owner was not told. Every owner that keeps an entry per slot then kept the entry for ever: `ioservice::Server`'s connection table through `LoopServer::Binding` (128 entries in the root's /boot, 4 in each `Outgoing`), echo's client table (4), and the root's `ResourceServer` and `LauncherServer` connection pools (4 each, five resource servers). After enough failures such a server answers `NoResource` to everyone. This is item 1 of `docs/repairs/261029/ioservice.md` and item 2 of `docs/repairs/261029/echo.md`, both deleted by this change; the root's pools had the same defect and were never listed.

**Cause**
Why `arm()` can fail at all. A `Waiter` never arms its next wait: it answers `Rearm` and `run()` arms (step 3), so that a `Waiter` cannot leave a slot with two waits or none. `arm()` is `object_wait_async`, and the kernel can refuse it: `NoMemory`, which is the realistic one: `HandleTable::attach_port_observer()` allocates a `PortObserver` on every arm and answers `NoMemory` when it cannot (`kernel/object/handle_table.cpp:218`), so a process under memory pressure meets it on any arm; `BadHandle`, from the same function, when a `Waiter` broke the rules of `Waiter`'s comment and closed or sent away its own handle and still answered `Rearm`; and in theory `BadType`, `AccessDenied` and `InvArgs`, from the same function, and `NoResource`, from `PortDispatcher::register_observer()` when the port is at its observer limit (`kernel/object/port_dispatcher.cpp:341`, reached through `Dispatcher::attach_port_observer()`, `kernel/object/dispatcher.cpp:119`), which the build-time `static_assert` on the table's size rules out. Step 4 of `run()` closed the handle and freed the slot for all of them, as it must: with no wait armed and its packet taken, the slot has nothing left to be called for.

Why only the `Waiter`'s owner can fix it. After the free, the Loop has nothing that names the entry: the slot is clear, and the key of the dead life matches nothing. The owner's entry (a `Connection` of the `Server`, a `ResourceConnection`, an echo `Client`) is reachable only through the `Waiter`, and the `Waiter` had been called for the last time. An owner cannot tell a re-arm that worked from one that did not: the answer `Rearm` is the same, and nothing is called when the wait fires again, since it will not. A poll would have to ask the Loop whether a slot is still in use, which is a second interface for what the Loop already knows at the one moment it matters. So the Loop tells it, once.

Why a second virtual function and not the other two options of the repair item (the maintainer chose the first). (b) treating a failed re-arm as an answer of `Close` would have to call `on_signals()` again or invent a call that looks like one, and a `Waiter` cannot tell "I answered Close" from "the Loop closed me"; its release paths are written for the first. (c) leaving it would keep the comment of `handle()` honest and the defect. A second function says the true thing: the slot is gone, and why.

Why the call comes last, after the close, the new generation and the free. Three reasons, each a test. The handle is closed, so its number may name another object by now: a `Waiter` told earlier could touch someone else's handle (`check_failed_rearm_is_told` checks that the fake kernel shows the handle closed and exactly one `handle_close` when the `Waiter` is told). The slot is free, so an `add()` from the callback takes it, even in a table that is full (`RunLetsTheWaiterAddFromOnRearmFailed` fills the table and replaces the slot's occupant from inside the call; moving the call before the slot is cleared fails it). And a packet of the dead life still queued behind is dropped by step 1's generation check and reaches neither the old nor the new `Waiter` (the same test queues one). The `Waiter` pointer and the status are therefore kept in locals before the slot is cleared, and `run()` does not touch `slot` after the call.

Why it is not called in the other three cases. After `Close` the `Waiter` has released what it held in the same call, and it cannot be told twice. When `add()` fails the caller has the status and keeps the handle (it may still send an epitaph on it), and no slot was ever in use. And in the Loop's destructor nothing is running and the owners of the `Waiter`s go with the Loop; calling them from a destructor would run owner code while the owner is half torn down.

Why the default does nothing, and the destructor and constructor are as they were. A `Waiter` that only ever answers `Close` (`OutgoingWaiter`, `TerminatedWaiter`) is never told, so it needs no override; an empty body also means a `Waiter` written before this change still compiles and behaves as it did. The destructor stays protected and non-virtual and the constructor `constexpr`, for the reasons of step 1.

**Change**
```cpp
// loop.hpp, class Waiter
virtual auto on_signals(ours::Signals observed) -> Action = 0;

virtual auto on_rearm_failed([[maybe_unused]] ours::Status const cause) -> void {}

// loop.cpp, run(): before
if (action == Action::Rearm && arm(key.index()) == ours::Status::Ok) {
    continue;
}
auto _ = ours::handle_close(slot.m_handle);
slot.m_handle = ours::Uandle::Invalid;
slot.m_signals = ours::Signals::None;
slot.m_waiter = nullptr;
++slot.m_generation;

// loop.cpp, run(): after
auto rearm = ours::Status::Ok;
if (action == Action::Rearm) {
    rearm = arm(key.index());
    if (rearm == ours::Status::Ok) {
        continue;
    }
}
auto &waiter = *slot.m_waiter;
auto _ = ours::handle_close(slot.m_handle);
slot.m_handle = ours::Uandle::Invalid;
slot.m_signals = ours::Signals::None;
slot.m_waiter = nullptr;
++slot.m_generation;
if (action == Action::Rearm) {
    waiter.on_rearm_failed(rearm);
}
```
Reaching step 4 with `action == Rearm` means the re-arm failed, so that condition is the whole test for "tell". `run()` makes no new system call (I1) and the order of the four steps is as it was; I3 now reads "for that slot's own Waiter's answer, or for the failure of the re-arm that answer asked for". The comment on the close in step 4, "the Loop owns the handle, so the close cannot be refused", gained its one exception: a `Waiter` that closed the handle itself earns `BadHandle` from `arm()`, and then the Loop's close is refused too, and nothing is to be done about that either.

What it costs. Per `Waiter` class: one slot in its vtable (8 bytes, and one relative relocation of 24 bytes in `.rela.dyn` for the function address). Per object: nothing; no `Waiter` grew, no field was added, and `.data` and `.bss` of both programs are byte-for-byte the same size. Per dispatch: nothing; `on_signals()` is called as before, and `on_rearm_failed()` is reached only on a failed re-arm. The default is one shared inline function (6 bytes in `init`). The programs measured, with `llvm-size -A` on the unstripped program and `llvm-objcopy --strip-all` for the file, built from `a8489d18` and from this tree with the same toolchain:

| `init` | Before | After | Change |
|---|---|---|---|
| `.text` | 108,878 | 109,358 | +480 |
| `.data` | 51,360 | 51,360 | 0 |
| `.bss` | 55,296 | 55,296 | 0 |
| `.data.rel.ro` | 2,296 | 2,624 | +328 |
| `.rela.dyn` | 5,712 | 6,696 | +984 |
| `.rodata` | 6,012 | 6,108 | +96 |
| stripped file | 177,776 | 179,656 | +1,880 |

| `echo_server` | Before | After | Change |
|---|---|---|---|
| `.text` | 25,294 | 25,470 | +176 |
| `.data` | none | none | |
| `.bss` | 22,960 | 22,960 | 0 |
| `.data.rel.ro` | 392 | 408 | +16 |
| `.rela.dyn` | 600 | 648 | +48 |
| stripped file | 30,240 | 30,512 | +272 |

Where `init`'s numbers come from (`llvm-nm -S` of both programs, `llvm-objdump`). `.text`: 434 bytes of functions and 46 of alignment. The new functions are `ResourceServer::rearm_failed` 154 and `LauncherServer::rearm_failed` 120 (each builds a debug line from several strings), `ResourceConnection::on_rearm_failed` 19, `LauncherConnection::on_rearm_failed` 15 and its thunk 16, `LoopServer::Binding::on_rearm_failed` 26, `LoopServer::rearm_failed` 15 and the shared default `loop::Waiter::on_rearm_failed` 6; `Loop::run()` grew from 313 to 379 bytes; `ours::to_string` shrank by 3. `.data.rel.ro` and `.rela.dyn`: of the 328 and 984 bytes, **48 and 144 are the second virtual function**: five vtables each grew by 8 bytes (`Binding`, `ResourceConnection`, `OutgoingWaiter`, `TerminatedWaiter`) or 16 (`LauncherConnection`, which has a slot and a thunk to its `Waiter` subobject), and each new slot is one relative relocation, 6 in all. **The other 280 and 840 bytes are not the Waiter.** They are a new 35-entry table of absolute addresses in `.data.rel.ro`, with its 35 relocations, that `ours::to_string(Status)` now indexes. `init` links one of three weak copies of that `inline` function (from `ort/start.cpp`, `launcher_server.cpp` and `realm.cpp`) and the copy it keeps changed from a table of 32-bit offsets in `.rodata` to a table of addresses; this commit added calls to `to_string` in `launcher_server.cpp`, and the compiler chose the other table there. I did not find why. It is recorded as `docs/repairs/261029/abi.md` item 1. `echo_server` keeps the first form of `to_string` (`.rela.dyn` +48 is its two new vtable slots), so its table is the Waiter's cost without it: +16 and +48 bytes of data and relocations, and +176 of code (`Client::on_rearm_failed` 49, `Binding` 26, `LoopServer::rearm_failed` 15, `run()` +66, and alignment). The file figures include the same: 1,120 of `init`'s 1,880 stripped bytes are `to_string`'s table and its relocations.

The step-6 section of this file measured the whole Waiter task as +64 bytes of `.data` + `.bss` in `init`; this change adds none to that figure.

Rejected: a pure virtual `on_rearm_failed()`, which every `Waiter` would have to write, including the two that answer only `Close` and the shell's `LoopClient`; and a flag in the answer (`Action::RearmOrDie`), which would let a `Waiter` choose to be told and still not release.

**Reference**
None, this project's own design: the second option of the repair item (`docs/repairs/261029/ioservice.md` item 1), chosen by the maintainer.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build, with `-Werror`) and `phony/benches/benches` build. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 27 passed (the 21 of before and six new ones), `host/obj/system/lib/ioservice/test/loop_server` 15 (13 and two), `.../ioservice/test/outgoing` 9, `.../ioservice/test/ioservice` 42, `host/obj/system/service/init/test/autorun` 5, all with 0 failed.
- Five deliberate breakages of `Loop::run()`, the source put back after each and compared with `cmp` against a saved copy. (M1) the call removed: 4 loop tests fail (`RunClosesTheSlotWhenTheRearmFails`, both `RunTellsTheWaiter...`, `RunLetsTheWaiterAddFromOnRearmFailed`). (M2) the guard `action == Rearm` removed, so a `Close` is told too: 5 fail (`RunClosesTheSlotWhenItsWaiterAnswersClose`, `...WithAMessageUnread`, `RunDoesNotTellAWaiterThatAnsweredClose`, `RunDropsAPacketForASlotFreedInItsNextLife`, `RunClosesOnlyTheSlotWhoseWaiterAnswersClose`). (M3) told before the handle is closed: 3 fail (both `RunTellsTheWaiter...` and `RunLetsTheWaiterAddFromOnRearmFailed`). (M4) told with `Ok` in place of the cause: 2 fail (both `RunTellsTheWaiter...`). (M5) told after the close but before the slot is cleared: 1 fails, `RunLetsTheWaiterAddFromOnRearmFailed`, and only it: it is the test that fills the table, and the other tests would not have noticed the order.
- clang-tidy against `out/bringup.x86`, `--header-filter` on the four directories, on `loop.cpp`, `loop_test.cpp`, `loop_server.cpp`, `loop_server_test.cpp`, `fake_kernel.cpp`, `resource_server.cpp`, `launcher_server.cpp`, `realm.cpp` and `echo_server.cpp`: exit 0, no diagnostic. `scripts/format_cpp.py --check` on every C++ file changed: clean. `scripts/lint_docs.py --changed` on each document: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-notify`, in `.worktrees/lw-notify` with the final tree: build ok in 7.4 s; dbg PASS (shell finished at 5.7 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.9 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 programs passed, 0 failed). `rel.log` holds `routing_checks: passed`, `init: echo exited with 0` and `pc-ps2: key a`, and both logs hold `shell: loop checks passed`. No log holds a `cannot wait on a client again` line, as expected: no program breaks the rules and the kernel never refused a re-arm in the boot.
- Not measured: the time of a dispatch (nothing on the path changed but `run()`'s step 4, which runs once per close), and anything on x86-64 hardware. The path itself is not run on the target: a failed re-arm needs the kernel to refuse `object_wait_async`, which it does for no program here; the host tests with the fake kernel are the proof of it.

**Deliberately not done**
- The shell's `check_loop()` has no check of the new path. The only way to reach it on the real kernel is a `Waiter` that breaks the rules (closes its own handle and answers `Rearm`, which earns `BadHandle`); that would be a check of the rule-breaking, and the host tests already pin the Loop's side with the fake.
- No notification at the Loop's destructor, and none for `add()`'s failure; see the Cause.
- `Delegate` (`LoopServer`) is not told: `write_failed()` exists so that a program can say a reply failed, and a failed re-arm is the Loop's, which the `Binding` answers by releasing. The owner that wants to log has the cause in `on_rearm_failed()`; the `ioservice` library has no debug-line dependency, so its `Binding` does not log.
- The repair items' numbers are not closed up: after the deletions `ioservice.md` has items 2 and 3 and `echo.md` items 1 and 3, because documents cite them by number.
- `docs/guide/` and `examples/hello` are not touched; another step updates them. The lines of `examples/echo/echo_server.cpp` that moved are listed in `.agents/tasks/loop-waiter-notify/RESULT.md`.

**How it was found**
None for the defect: it was recorded as latent when `LoopServer::Binding` and echo's `Client` became `Waiter`s (`ioservice.md` and `echo.md` of this day). The `to_string` table was found by looking for what the 984 bytes of `.rela.dyn` were: `llvm-readelf -r` counted 214 relative relocations before and 255 after, `llvm-nm -S` showed that only five vtables changed size (48 bytes), `llvm-nm`'s gaps in `.data.rel.ro` showed a 280-byte object with no symbol that had not been there, and `llvm-objdump -d` showed `ours::to_string` as the one function that loads from it, with the baseline's copy loading 32-bit offsets from `.rodata`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `run()` calls `on_rearm_failed()` after the slot is cleared and its generation bumped, not before. Which three things go wrong if it is called earlier, and which test fails when the call sits between the close and the clearing?
2. Why is a `Waiter` not told when `add()` fails, when the Loop's destructor frees its slots, or after its own `Close`, although in each the slot ends free?
3. The second virtual function adds 8 bytes to a vtable, yet `init`'s `.rela.dyn` grew by 984 bytes. Which part is the Waiter's, which is not, and how can the same inline function compile to two different tables in two translation units?
