# benches changes

## Title: loop-dispatch-bench times a Loop slot's callback as a function pointer, a virtual call and a function reference (2026-10-28)

**Files**
- `benches/system/lib/loop/loop_dispatch_bench.cpp` — new: the host benchmark
- `benches/system/lib/loop/BUILD.gn` — new: `loop-dispatch-bench`, host toolchain, depending only on `//system/lib/loop:headers`
- `benches/BUILD.gn` — `//benches:benches` builds it
- `docs/perf/261028/loop-dispatch.md`, `docs/perf/261028/loop-dispatch/` — the report and its raw samples (`baseline.csv`, and `a.csv`, `b.csv`, `c.csv`, which are filtered copies of it)

**Symptom**
None, a new benchmark. After `LoopServer::Delegate`, `Outgoing`'s `Handler` and `LauncherOps` became abstract classes, the user asked about the same change for `loop::Callback`, the one that is on every packet's path, whose comment says "never a virtual call". Its price had not been measured, and the user asked for the benchmark first (2026-10-28).

**Cause**
None, a new benchmark.

**Change**
The bench does not run the Loop. It mirrors the one step of `Loop::run()` that differs between the forms, finding the slot and making the call, so that the difference is the call and not a fake kernel's bookkeeping. Objects:

- **Three forms over the same six objects.** A: `slot.callback(slot.context, observed)`, today's `loop::Callback`. B: `slot.waiter->on_signals(observed)`, a virtual call on an abstract class. C: an object pointer and a function pointer to a thunk the compiler writes for a member function, which is what a non-owning function reference is.
- **Six different target classes**, `Target<0>` to `Target<5>`, one per kind of registration the root has, so each call site has six targets and cannot be devirtualized; the order of the calls is read from a table at run time. Each target reads its own member and does two bit operations.
- **Three orders:** `same` (always slot 0, a predictor that is always right), `cycle` (0 to 5 in turn) and `random` (a fixed pseudo-random order over 65536 entries, which the predictor cannot learn).
- **Two yardsticks:** a direct call to a function not inlined, and one `write(2)` of a byte to `/dev/null`, the cheapest real system call the host has.
- **`check`** runs one short round and compares the three forms' totals, so that they are known to do the same work; **`csv`** prints every raw sample in the format `scripts/perf_stats.py` reads.

Two mistakes of the bench's own, found by reading its first output and fixed before any sample was kept: the direct-call yardstick read 0.000 ns, because the call reads memory and nothing in its loop changes it, so the compiler made it once (an empty `asm volatile` with a memory clobber in the loop stops that); and A always ran first in a round, which put A and C, the same machine code, 0.38 ns apart on the random order (the variant that goes first now rotates from round to round).

**Reference**
None, this project's own measurement.

**Verification**
- `loop-dispatch-bench check`: `check ok`.
- `llvm-objdump` of the built bench: A's and C's loops each have one `blr` through a pointer loaded from the table; B's has two dependent loads (the object's vtable pointer, then the slot) before its `blr`; the direct yardstick is a `bl`. Nothing was inlined or devirtualized.
- clang-tidy on the bench with the build's compile database: no diagnostic after `judge` was marked `[[nodiscard]]`. clang-format and gn format: clean.
- The numbers, `docs/perf/261028/loop-dispatch.md`: 90 samples a case; B costs the same as A when the predictor is right and 0.76 ns (13.5%) more when it is wrong; C is not distinguishable from A; one `write(2)` takes 579.8 ns.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-loop-dispatch-bench`, at HEAD): build ok in 6.9 s; dbg PASS, shell finished at 5.6 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.7 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. The benchmark is host-only and outside both images and the host-test list, so this shows only that adding it broke nothing; its own build and `check ok` were run in the same worktree.

**Deliberately not done**
- Nothing in the Loop or its users is changed. The report does not choose between `Waiter` and a function reference.
- No number from x86-64 hardware or from the kernel's own system calls: this host is arm64, and no emulator stands in for either. The report says so.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the bench time the call site of three loops of its own and not `Loop::run()` on the fake kernel?
2. B's two extra loads cost nothing in the `same` and `cycle` orders and 0.76 ns in `random`. What is the processor doing in the second case that it is not doing in the first?
3. A and C are the same machine code, yet they differed by 0.38 ns in the first runs. What does that tell you about how far to trust a difference of that size, and what did the bench do about it?
