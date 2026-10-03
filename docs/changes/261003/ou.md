# ou changes

## Title: ou passes every option through, and the shell calls nothing but ou (system-abi-surface step 5, part 1) (2026-10-03)

**Files**
- `system/lib/ou/include/ou/channel.hpp`, `process.hpp`, `thread.hpp`, `src/channel.cpp`, `process.cpp`, `thread.cpp` — `channel_write`, `channel_read`, `process_create` and `thread_create` take their options word as a last parameter, defaulting to None, and pass it through.
- `examples/shell/shell.cpp`, `BUILD.gn` — the 18 direct `ours_*` calls go through `ou`; `<ours/vdso.hpp>` is no longer included.
- `system/lib/ou/README.md`, `.agents/skills/ours-kernel-design/references/layering.md`, `docs/design/260923/userspace-interface.md` — the rule now holds in the tree; an English note beside the Chinese text.

**Symptom**
The rule is that ring 3 calls `ou::`, never the raw `ours_*` symbols. One program broke it: the shell, whose ABI checks made 18 direct calls. Nothing stopped any other program from doing the same.

**Cause**
The objects:
- `ours_*`: the vDSO's raw entry points, declared in `<ours/vdso.hpp>`.
- `ou`: the library that names each of them for a program.
- The shell's checks of the raw ABI, which hand the kernel what it must refuse: an option bit, a pointer into nothing, into the kernel half, or into the read-only vDSO.

The events:
1. `ou`'s four entries for calls with an options word fixed it to None, since None is the only value the kernel accepts.
2. A check that the kernel refuses any other value could not be written through `ou`, so it called `ours_*` directly.
3. Once one program called the raw entries, the bad-pointer checks did too, though `ou` already passed pointers through unchanged. With nothing in the build to say otherwise, `ou` was optional.

The user decided on 2026-10-02 against an exception list for tests: everything goes through `ou`, so `ou` must pass every ABI parameter through.

**Change**

```cpp
// ou/thread.hpp, before
auto thread_create(ours::HandleValue process, Str name, ours::HandleValue *out) -> ours::Status;
// after
auto thread_create(ours::HandleValue process, Str name, ours::HandleValue *out,
                   ours::ThreadCreateOptions options = ours::ThreadCreateOptions::None)
    -> ours::Status;

// the shell's check, before
ours_thread_create(process, NAME.data(), NAME.size(), static_cast<ours::ThreadCreateOptions>(1), &thread)
// after
ou::thread_create(process, NAME, &thread, static_cast<ours::ThreadCreateOptions>(1))
```

The options go last, with a default, so that no existing caller changes. The other 16 calls needed no change to `ou`: they pass bad pointers, and `ou` forwards a pointer as given.

**Reference**
Zircon's own core tests call `zx_*` directly; this is the deliberate departure the user chose. The shape of the entries, each with every parameter of its call, is that of Zircon's `lib/zx` wrappers, which take the options word: `zx::channel::create(uint32_t flags, ...)`, `zx::thread::create(const process&, const char*, uint32_t, uint32_t flags, ...)`.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- The checks whose calls moved, by name in the logs: "bad-pointer checks passed" and "handle_duplicate checks passed" in both images; "process_create checks passed" and "thread_create checks passed" in dbg, skipped in rel as before.
- `grep` finds no `ours_` call in `examples/shell/shell.cpp`.

**Deliberately not done**
- Enforcement: GN visibility so that only `ou` can depend on the raw declarations, and an `#error` in `<ours/vdso.hpp>` for anyone else. That is part 2, its own commit.

**How it was found**
Step 5 of `.agents/tasks/system-abi-surface/DESIGN.md`.

**The user's prediction**
None; the user approved the step without one.

**Three questions you should be able to answer**
- Why could the bad-pointer checks move to `ou` with no change to `ou`, while the option-bit checks could not?
- Why do the options go last, with a default, rather than where the raw call has them?
- What does a rule like "ring 3 calls only `ou`" need besides every program following it today?

## Title: ou stays a convenience layer; the build does not enforce it, and the typed layer is deferred (system-abi-surface steps 5 and 9) (2026-10-03)

**Files**
- `.agents/tasks/system-abi-surface/DESIGN.md` — the goal "only through `ou`, enforced" dropped; step 5's second commit dropped; step 9 (generated, typed `ou`) and the parts of step 8 only it needs deferred; the decision rows updated.
- `.agents/skills/ours-kernel-design/references/layering.md`, `system/lib/ou/README.md` — `ou` by convention, not enforced.

**Symptom**
The plan was to gate `<ours/vdso.hpp>` in the build so that only `ou` could include it, and then to generate `ou` with typed handles. Asked what `ou` adds over `ours_*` today, the honest answer was little: most entries forward unchanged. The value was all in the typed layer still to come.

**Cause**
The objects:
- `ours_*`: the vDSO's C ABI, fixed once published. It cannot go, and `ou` is built on it.
- `ou`: a C++ layer that can change freely. Today it adds `Str`, option defaults and `debug_write`'s splitting.
- The enforcement: GN visibility on the generated declarations plus an `#error` in `<ours/vdso.hpp>`. It guards against a mistake, not an attack: a program can always declare an `ours_*` symbol itself.

The events:
1. The user asked why there are two layers if they hardly differ.
2. Of three ways forward, the user kept `ou` and declined to add build machinery or a generator until a concrete need for them appears: keep `ou` as it is, unenforced (A); remove `ou` (B); or enforce it and generate a typed `ou` (C).

**Change**
Documents only. The code is as the previous section left it: every `ou` entry passes every parameter through, and the shell calls nothing but `ou`.

**Reference**
Zircon's arrangement: `zx_*` from the vDSO, `lib/zx` as an optional C++ layer, and nothing in the build that stops a program, its own core tests included, from calling `zx_*` directly.

**Verification**
- `lint_docs.py` on the changed documents. No code changed.

**Deliberately not done**
- The second commit of step 5 (the GN visibility and the `#error`) and step 9. The previous section's "Deliberately not done", which named the enforcement as the next commit, no longer holds.

**How it was found**
The user's question, before step 5's second commit: what `ou` is for, if `ours_*` already exists.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `ours_*` never be removed, while `ou` can change at any time?
- What would a typed `ou` catch at compile time that `ours_*` cannot, and what concrete problem would justify building it?
- Why is a build-time gate on `<ours/vdso.hpp>` a guard against mistakes and not a security boundary?

## Title: Everything but the shell calls ours_* where it called an ou forwarder (remove-ou, step 1) (2026-10-03)

**Files**
- `system/lib/runtime/src/start.cpp`, `system/lib/launch/src/launch.cpp`, `system/service/init/init.cpp`, `kernel/lib/userabi/userboot/userboot.cpp`, `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — 41 calls of `ou`'s forwarders now call the `ours_*` entry directly, and include `<ours/vdso.hpp>` instead of the `ou` headers they no longer use.
- `.agents/tasks/remove-ou/PLAN.md` — new: the plan for removing `ou`.

**Symptom**
`ou` was a second copy of the system-call API: one forwarding function per call, with its contract restated by hand in its headers. Adding a call meant writing the forwarder too, and the restated contracts drifted from `syscalls.gni` (several of the false statements step 1 of system-abi-surface corrected were in `ou`'s headers).

**Cause**
The objects:
- `ours::syscall::*`: the kernel's handlers.
- `ours_*`: the vDSO's exported entry points, which are the user-level API.
- `ou::*`: C++ functions that, apart from a few helpers, only called the matching `ours_*`, in a different argument order and with defaults.

The events:
1. `ou` was written as the one place a program would learn how to reach the kernel, and to grow typed handles later.
2. The typed layer was deferred (the user, 2026-10-03), and what was left was a forwarder per call.
3. The user decided to remove it: a layer that adds nothing is one more thing to keep true.

**Change**
A script rewrote each call by the forwarder's own definition: the same arguments for the calls `ou` passed straight through, and for the others the raw order with `ou`'s defaults written out.

```cpp
// before
ou::channel_read(here, buffer, sizeof(buffer), &actual);
// after
ours_channel_read(here, ours::ChannelReadOptions::None, buffer, nullptr, sizeof(buffer), 0, &actual,
                  nullptr);
```

A name passed as an `ou::Str` becomes the pointer and the length the raw call takes. The helpers that are not forwarders (`ou::debug_line`, `ou::decimal`, `ou::Str`) stay until step 3 moves them.

**Reference**
Zircon programs call `zx_*` directly from C and C++. `lib/zx` is an optional C++ layer over it, which this tree no longer has.

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The init-shell exchange and the launcher's checks both run through the rewritten calls.

**Deliberately not done**
- The shell (step 2) and the removal of `ou` itself (step 3), each its own commit.
- Two comments that describe `ou`'s design (`runtime/startup.hpp`, `launch.cpp`'s `Owned`): they go with `ou` in step 3.

**How it was found**
The user's question of what `ou` adds over `ours_*`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why could the rewrite be done by a script, and what had it to know about each forwarder?
- What did `ou::channel_read` hide that the raw call makes the caller write?
- Where else in the tree was a call's contract written down, and which copy is now the only one?
