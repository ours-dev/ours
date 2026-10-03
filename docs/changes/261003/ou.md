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
