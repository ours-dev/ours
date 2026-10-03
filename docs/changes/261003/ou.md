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

## Title: The shell calls ours_* where it called an ou forwarder (remove-ou, step 2) (2026-10-03)

**Files**
- `examples/shell/shell.cpp` — 427 calls of `ou`'s forwarders now call the `ours_*` entry directly; it includes `<ours/vdso.hpp>` and only the `ou` headers of the print helpers; the comments that said its calls go through `ou` say they do not.
- `examples/shell/BUILD.gn` — the same, in the comments on its dependencies.

**Symptom**
As for step 1: the shell, the largest caller, reached the kernel through a layer of forwarders the user decided to remove.

**Cause**
See step 1. One thing new here: the first run of the rewriting script left 68 calls behind. It took the `'` of a digit separator (`0xffff'ffff'ffff'0000`, in `check_vmo_read()`) for the start of a character literal, and skipped what followed as if it were quoted. Because `ou` still existed, every call it skipped still compiled. They surfaced only because the step also removed the `ou` headers they needed. With the separator recognised, the second run rewrote them, and a check of every file from step 1 found no call left behind there.

**Change**
The same rewrite as step 1. A name given as a string literal becomes the literal and its length, `"host", sizeof("host") - 1`, at the 10 places that did so.

**Reference**
None beyond step 1.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- The shell's own checks, counted in the logs: 19 groups passed in dbg; in rel 13 passed and 5 skipped, as before.
- No call of an `ou` forwarder is left anywhere outside `//system/lib/ou`.

**Deliberately not done**
- Removing `ou` and moving its print helpers (step 3).

**How it was found**
Step 2 of `.agents/tasks/remove-ou/PLAN.md`; the missed calls by the build, once their headers were gone.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did the calls the script missed still build, and what made them fail at last?
- How does a digit separator differ from a character literal to something that reads C++ one character at a time?
- Which of `ours_channel_read`'s arguments did `ou::channel_read` let a caller leave out, and what did it pass for them?

## Title: ou is removed; its print helpers live in the runtime, on std::string_view (remove-ou, step 3) (2026-10-03)

**Files**
- `system/lib/ou/` — deleted.
- `system/lib/runtime/include/runtime/debug.hpp`, `src/debug.cpp`, `BUILD.gn` — new: `runtime::debug_write`, `debug_line`, `decimal` and `hex`, as they were in `ou`, taking and returning `std::string_view`. They are part of the runtime source set.
- `system/lib/runtime/src/start.cpp`, `system/service/init/init.cpp`, `kernel/lib/userabi/userboot/userboot.cpp`, `kernel/lib/syscall/test/fault_probe/fault_probe.cpp`, `examples/shell/shell.cpp` — `runtime::` for the helpers, `std::string_view` for `ou::Str`; the two bounded reads of a NUL-terminated buffer (`Str::from_c_str`, `ou::command_text`) are `std::find` over the buffer.
- `system/lib/launch/BUILD.gn`, `system/service/init/BUILD.gn`, `kernel/lib/userabi/userboot/BUILD.gn`, `kernel/lib/syscall/test/fault_probe/BUILD.gn`, `examples/shell/BUILD.gn` — no dependency on `//system/lib/ou`.
- Comments that named `ou`: `system/BUILD.gn`, `system/user_executable.gni`, `system/public/ours/vdso.hpp`, `system/public/ours/syscalls.gni`, `build/abi/gen_syscalls.py`, `kernel/lib/userabi/src/userboot.cpp`, `system/lib/launch/src/launch.cpp`, `system/lib/runtime/include/runtime/startup.hpp`.
- `.agents/skills/ours-kernel-design/references/layering.md`, `SKILL.md` — ring 3 calls `ours_*`; adding a call is the description entry and the handler.
- `.agents/tasks/system-abi-surface/DESIGN.md`, `.agents/tasks/prof-sampler-object/PLAN.md`, `DESIGN.md`, `docs/design/260923/userspace-interface.md` — a note that `ou` is gone.
- `.agents/tasks/remove-ou/PLAN.md`, `RESULT.md` — done.

**Symptom**
After steps 1 and 2, `ou` held nothing but four print helpers and `Str`, a hand-written string view.

**Cause**
`Str` existed because a ring-3 program once linked no C library, and `std::string_view{"text"}` needs `strlen`. Every program now links `//system/lib/libc`, so `ou`'s own README named "make `Str` an alias of `std::string_view`" as the step left. The helpers are not a system-call layer: they make `ours_debug_write` fit to call. They split text at `MAX_DEBUG_WRITE`, assemble a line so it goes out in one call, and format numbers with no printf to do it.

**Change**
The helpers moved to the runtime, which every program links. A separate target was tried first, on the belief that userboot had an entry point of its own and could not link the runtime's `crt0.S`. It has not: userboot runs on the runtime like every other program. So the runtime's own BUILD.gn comment saying otherwise was stale too, and is corrected. `Str` became `std::string_view` (the user's choice).

```cpp
// before
static_cast<void>(ou::debug_line({what, ou::Str::from_c_str(ours::to_string(status), 32)}));
// after
static_cast<void>(runtime::debug_line({what, std::string_view{ours::to_string(status)}}));
```

`ours::to_string()` returns a string literal, so the bounded scan `from_c_str` did was not needed there. The two buffers another program or the kernel wrote keep their bound, by `std::find` up to the buffer's end: the shell's command registry, and fault_probe's probe name.

**Reference**
Zircon's programs print through libc's `printf` over `zx_debug_write`. Here there is no printf, so the runtime keeps the line-at-once helper.

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error. Every ring-3 program, the launcher library and the host tests build.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The shell's groups: 19 passed in dbg; 13 passed and 5 skipped in rel, as before. fault_probe ran every probe in dbg.
- init builds, but neither image starts it (userboot starts the shell), so its changes are checked by the compiler only.
- No `ou::`, `<ou/` or `//system/lib/ou` is left in code or build files, except three comments saying where the helpers came from.

**Deliberately not done**
- The change records and old task logs that name `ou` are history and stay as written.

**How it was found**
Step 3 of `.agents/tasks/remove-ou/PLAN.md`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why could `Str` become `std::string_view` now, when it could not when `ou` was written?
- Why do the shell's registry and fault_probe's probe name still need a bounded scan, while a status name does not?
- What does `runtime::debug_line` do that a bare `ours_debug_write` per piece would get wrong?
