# loop-dispatch: performance report

**Date**: 2026-10-28
**Status**: Done

## Question
What it costs to call a Loop slot's callback in three forms, and whether the extra cost of the one with a `virtual` is enough, beside the rest of a packet's work, to decide anything. The forms are **A**, today's `loop::Callback`, a function pointer and a `void *context`; **B**, a virtual call on an abstract `Waiter` that a slot would point at; and **C**, a non-owning function reference, which is an object pointer beside a function pointer to a thunk the compiler writes for a member function.

`loop::Callback`'s comment says "never a virtual call"; this puts a price on that sentence. It answers the user's question of 2026-10-28, asked after the other function-pointer-and-context structs of the component framework were replaced by abstract classes (`docs/changes/261028/ioservice.md`, `svc.md`, `launch.md`): is `Waiter` (B) or a function reference (C) worth it for the Loop, the one place where the call sits on every packet's path? The user chose to measure first. Nothing is changed by this report; it has no repair item.

## Method
- **Machine and build:** Apple M5 Pro, macOS 27.0.1, clang 23.1.0-rc3 at -O2, the host toolchain. That is an **arm64** processor, and the kernel runs on x86-64: these numbers say how A, B and C compare on this processor, not what a call costs on the target. No emulator can stand in for either, since it models neither the predictor nor the load latency. They are marked as a host number, not a number of the kernel's hardware.
- **Benchmark:** `benches/system/lib/loop/loop_dispatch_bench.cpp`. It mirrors the one step of `Loop::run()` that differs between the forms, finding the slot and making the call, and nothing else. Each form has a table of six slots, each over a different class (six, as the root has six kinds of registration), so the call site has six targets, as `run()`'s has. The order the slots are called in is read from a table at run time, so the compiler cannot tell which target a call goes to. Every target does the same few instructions: a read of its own member, two bit operations, and an answer of `loop::Action`. The three forms return the same totals, which the bench checks on every round (`loop-dispatch-bench check`).
- **What the machine code is** (`llvm-objdump` of the built bench): A and C are the same loop, one load of the pointer from the table and one `blr`. B loads the object's pointer, then its vtable pointer, then the slot of the vtable, and then `blr`: two dependent loads more. The direct yardstick is a plain `bl`. Nothing was inlined or devirtualized.
- **Cases:** the order the slots are called in. `same`: always slot 0, so the branch predictor is right every time and the cost is that of the call itself. `cycle`: slots 0 to 5 in turn, which the predictor can learn. `random`: a fixed pseudo-random order over a table of 65536 entries, which it cannot learn and which makes the call site guess wrong now and then, as a server with several kinds of client that talk in no pattern would.
- **Metrics:** `ns/dispatch`, host time per call of a callback, from a round of 4194304 calls timed as a whole (the host clock ticks every 41.7 ns). Two yardsticks: `direct call`, the same loop calling a function by name that the compiler is told not to inline, the cost of calling at all; and `write(2) to /dev/null`, one write of a byte, the cheapest real system call the host has, in ns/call.
- **Samples:** 30 rounds a run, run three times, the three runs' samples together: 90 a case. Within a round the three forms run one after another and which goes first rotates from round to round. One untimed warm-up first. Raw samples: `loop-dispatch/baseline.csv`. `a.csv`, `b.csv` and `c.csv` are copies of it with one form's rows each, unchanged but for the prefix of the case name, so that `perf_stats.py --compare` can compare two forms.
- **Command:** `prebuilt/ninja/ninja -C out/<dir> host/obj/benches/system/lib/loop/loop-dispatch-bench`, then `out/<dir>/host/obj/benches/system/lib/loop/loop-dispatch-bench csv`, three times.
- **Two mistakes of the bench itself, found and fixed before these samples:** the first direct-call yardstick read 0.000 ns, because the call reads memory and nothing in the loop changes it, so the compiler made it once; an empty `asm volatile` with a memory clobber in the loop stops that. And the first runs always ran A first in a round, and put A and C, which are the same machine code, 0.38 ns apart on the random order, so a position in the round has a cost of its own; rotating the order took that down to the 0.2 ns below.

## Baseline
`loop-dispatch/baseline.csv`, at the tree of 5806dc2e:

| case | metric | unit | n | min | median | p90 | max | mean | stddev |
|---|---|---|---|---|---|---|---|---|---|
| A same | ns/dispatch | ns | 90 | 0.52 | 0.68 | 0.71 | 0.82 | 0.68 | 0.0407 |
| B same | ns/dispatch | ns | 90 | 0.52 | 0.67 | 0.71 | 1.07 | 0.68 | 0.0536 |
| C same | ns/dispatch | ns | 90 | 0.51 | 0.68 | 0.70 | 0.86 | 0.68 | 0.0368 |
| A cycle | ns/dispatch | ns | 90 | 0.65 | 0.68 | 0.72 | 0.82 | 0.69 | 0.0283 |
| B cycle | ns/dispatch | ns | 90 | 0.65 | 0.69 | 0.73 | 1.08 | 0.70 | 0.0515 |
| C cycle | ns/dispatch | ns | 90 | 0.65 | 0.68 | 0.72 | 0.84 | 0.69 | 0.0334 |
| A random | ns/dispatch | ns | 90 | 5.50 | 5.60 | 6.04 | 6.97 | 5.71 | 0.26 |
| B random | ns/dispatch | ns | 90 | 6.26 | 6.36 | 6.72 | 7.27 | 6.45 | 0.22 |
| C random | ns/dispatch | ns | 90 | 5.61 | 5.80 | 6.08 | 7.22 | 5.86 | 0.26 |
| yardstick direct call | ns/dispatch | ns | 90 | 0.43 | 0.45 | 0.47 | 0.74 | 0.46 | 0.0462 |
| yardstick write(2) to /dev/null | ns/call | ns | 90 | 565.6 | 579.8 | 593.4 | 746.5 | 585.8 | 25.3 |

B against A, `perf_stats.py --compare loop-dispatch/a.csv loop-dispatch/b.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| same | ns/dispatch | ns | 0.68 | 0.67 | 1.01x (within noise) | -1.3% |
| cycle | ns/dispatch | ns | 0.68 | 0.69 | 0.99x (within noise) | +0.6% |
| random | ns/dispatch | ns | 5.60 | 6.36 | 0.88x | +13.5% |
| yardstick direct call | ns/dispatch | ns | 0.45 | 0.45 | 1.00x (within noise) | +0.0% |
| yardstick write(2) to /dev/null | ns/call | ns | 579.8 | 579.8 | 1.00x (within noise) | +0.0% |

C against A, `perf_stats.py --compare loop-dispatch/a.csv loop-dispatch/c.csv`:

| case | metric | unit | median before | median after | ratio | change |
|---|---|---|---|---|---|---|
| same | ns/dispatch | ns | 0.68 | 0.68 | 1.01x (within noise) | -0.7% |
| cycle | ns/dispatch | ns | 0.68 | 0.68 | 1.00x (within noise) | -0.5% |
| random | ns/dispatch | ns | 5.60 | 5.80 | 0.97x (within noise) | +3.5% |
| yardstick direct call | ns/dispatch | ns | 0.45 | 0.45 | 1.00x (within noise) | +0.0% |
| yardstick write(2) to /dev/null | ns/call | ns | 579.8 | 579.8 | 1.00x (within noise) | +0.0% |

What they say:

- **When the predictor is right (`same`, `cycle`) the three forms cost the same**: about 0.68 ns a call, a direct call plus the table lookup. The extra loads of B cost nothing there, because they are independent of everything else and hit the cache while the processor is busy with other work.
- **When it is wrong (`random`) B costs more: 0.76 ns, 13.5%**, outside the noise. The processor cannot start fetching the right target until the two loads have given it the address, and after a wrong guess it has nothing else to do while it waits. This is the only case that shows it.
- **C is not distinguishable from A** in any case. A and C are the same machine code, so the 0.20 ns by which C is slower on the random order is what this bench cannot resolve (layout, which core, what ran just before); B's excess is about four times that.
- **In proportion**, one write(2) of a byte to /dev/null takes 579.8 ns on this host (the median above). B's worst excess, 0.76 ns, is 0.13% of one. A packet of the Loop's makes at least three system calls of the kernel's own (`port_wait`, `channel_read`, `channel_write`, and one more to arm the wait again), so the excess is about 0.04% of the system calls alone of one packet, if the kernel's system calls cost anything like the host's. That last "if" is not measured: no host call stands for the kernel's exactly.

## Stages
None. A, B and C are the candidates, not stages: they ran together so that they saw the same machine, and their comparison is in the baseline above. Nothing in the Loop had been changed when these samples were taken; what the Loop became is in the Decision below.

## Conclusion
- On this processor the cost of B over A is zero when the target is predictable and 0.76 ns when it is not, and C costs the same as A. Beside one system call (about 580 ns on the host), neither is a reason to keep a function pointer on the Loop's path.
- **Not measured:** the same bench on x86-64 hardware, where the load latency and the predictor are another's; the kernel's own system calls, the one number that would complete the proportion; and the Loop's other work per packet (the key decoded, the slot's generation checked, the wait armed again), which only widens the gap. What B saves is not a time: the callback part of a slot goes from 16 bytes to 8, and the 6 trampolines go. The 8 bytes a slot are a gross saving, and 8 KiB only in a table of `PORT_MAX_OBSERVERS` (1024) slots, which no program has; each registered object gains a vptr, so the net, as measured after the change, is in the Decision below.
- **What was left to decide** was not the cost but the shape: B (`Loop::Waiter`) has an abstract class, and `Realm`'s two registrations on one `Instance` need two small waiter members each; C (`ustl::FunctionRef`) keeps `Realm` as it is and needs a new type, since libc++ here does not enable `std::function_ref`. The report did not choose between them; the user did, below.

### Decision
The user chose B on 2026-10-29: a slot holds a `loop::Waiter`, an abstract class, and the function pointer and its `void *` context are gone from the Loop (`.agents/tasks/loop-waiter/PLAN.md`, six steps; the change records are the `loop-waiter` sections of `docs/changes/261029/loop.md`, `ioservice.md`, `init.md` and `echo.md`).

The recommendation put to the user with this report had been C, the function reference (the Decisions table of that plan: it removes the six trampolines and the casts, keeps a slot at 8 bytes and leaves `Realm` as it is). The report itself, above, left the choice open, since its numbers separated B from C only by B's 0.76 ns on a mispredicted target. The user chose B instead.

What B cost, from the change records of steps 1 to 5 and the account in `docs/changes/261029/loop.md`:
- A vptr in every registered object: `LoopServer::Binding` 16 to 24 bytes, `ResourceConnection` 16 to 24, `LauncherConnection` 5,112 to 5,120, echo's `Client` 16 to 24. `Instance` 224 to 256: it has two waits with two behaviours, so it cannot be the Waiter of both, and it holds two waiter members of 16 bytes (a vptr and a back pointer) and an `attach()` to set the pointers, since `Realm` resets an entry by assignment.
- A cost the plan did not expect. A `constinit` pool of Waiters is no longer all zeros, so the five `ResourceServer::Storage` objects moved from `.bss` to `.data`, and `init`'s stripped file grew by 22,432 bytes of `.data`, nearly all zeros (23,664 in all, with the vptrs' relocations and code). It is recorded and not fixed: `docs/repairs/261029/init.md` item 1.
- A second indirect branch on every call during steps 1 to 5, which carried the Waiter over the old form; step 6 removed it. The time of the real `Loop::run()` was not measured before or after: the B against A figures above are the only timing, and they are a host (arm64) number.
- No machine code saved at the six sites: each trampoline became an `on_signals()` of about the same size (the Launcher's connection also needed a `this`-adjusting thunk, 23 bytes), and `.text` grew where the objects are constructed, which now store a vptr: by 32 bytes for the Resource and Launcher connections, 432 for `Realm` and 48 for echo (steps 3 to 5).

What B saved, measured on the programs of three trees (before the task, before step 6, after it; `llvm-size -A`, x86-64):
- 8 bytes in each slot, `Slot` 32 to 24: 1,408 bytes in `init`'s 176 slots and 64 in echo's 8. That is the whole of the "8 KiB" for these programs, and it is gross.
- The net, once each registered object paid its vptr: `init`'s `.data` + `.bss` is 64 bytes larger over the whole task (1,472 paid, 1,408 saved) and echo's is the same size. Per connection the vptr and the slot's saving cancel (0); per `Instance` it is +16.
- `.text` falls by 400 bytes in each program, because the Loop carries one `add()` and no `call_waiter()`; over the task it is +384 bytes in `init` and -48 in echo.
- The six trampolines and their casts from `void *`, which was the reason for B.

So the memory saving this report's Conclusion first named is not there net: the plan gave the interface and the removed casts as the reasons for B, not memory, and the measured memory bill of B is +64 bytes of RAM in `init` and a file 23,664 bytes larger, nearly all of it the repair item above.
