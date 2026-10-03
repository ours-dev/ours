# abi changes

## Title: Status is [[nodiscard]] (system-abi-surface step 2, the attribute) (2026-10-03)

**Files**
- `system/public/ours/status.hpp` — `enum class [[nodiscard]] Status : u32`, with a comment saying how a deliberate drop is written.
- `.agents/tasks/system-abi-surface/DESIGN.md` — step 2 marked done.

**Symptom**
A function that answers a Status could be called as if it answered nothing, and nothing said so. The count before this step found 44 such calls, all in the kernel. Some were safe, some rested on an assumption nothing checked, and a few hid a real gap: an unimplemented preemption, an `activate()` failure, an IOAPIC EOI that is never sent.

**Cause**
The objects:
- `Status`, the one error type of both the kernel and the system-call ABI.
- A call that returns one, and the caller that ignores the answer.
- The compiler, which only warns about an ignored value when the function or its return type asks it to.

The events:
1. A function returns a Status.
2. A caller writes `f();`, because it thinks f cannot fail here, or because it does not know what to do if it does.
3. Nothing marks the difference between those two reasons, so a later reader cannot tell a considered drop from a forgotten one, and a later change to f that adds a failure reaches no caller.

**Change**

```cpp
// before
enum class Status : u32 {
// after
enum class [[nodiscard]] Status : u32 {
```

On the type, every function returning a Status by value is marked at once, including ones written later. The 44 drops were answered in the four commits before this one, module by module (mm, kernel/task, top with kshell and object, then time with arch, driver and ktl), each in one of three ways:
- `static_cast<void>(...)` with the reason, when the callee only ever answers Ok, or when an error changes nothing for this caller;
- `[[maybe_unused]] auto const status = ...; DEBUG_ASSERT(Status::Ok == status)`, when the callee can fail but not in this caller's state;
- real handling, where the drop was a bug: `VmAspace::create()` returning `init()`'s failure, `protect_locked()` keeping its first failure, `MemCache::init()` panicking, and `ClockEventDevice::enroll()` returning void.

**Reference**
Zircon's `zx_status_t` is `typedef int32_t`, a C type, and standard `[[nodiscard]]` applies to functions, classes and enumerations, not to a typedef. So Zircon marks functions one at a time, `[[nodiscard]] zx_status_t Foo()`, which covers only the functions someone remembered to mark. An enum class can carry the attribute itself, which is why this design chose the type (decision of 2026-10-02 in the design).

**Verification**
- After the attribute, every target in `out/vmo-snapshot` rebuilt with `ninja -k 0`: the kernel in both configurations, phys, the host tools and ring 3. There were 444 steps and no unused-result diagnostic; the only warnings were the known `-lto_library` linker notes.
- A snippet compiled with the kernel's own flags: a bare `f();` that drops a Status is `error: ignoring return value of type 'Status' declared with 'nodiscard' attribute [-Werror,-Wunused-value]`, and `static_cast<void>(f())` compiles.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).

**Deliberately not done**
- Nothing in `ou`: its entries return `ours::Status`, so the type already covers them, and the generated `ou` of step 9 will be covered the same way.
- The status codes themselves (NotFound against NoResource, NoSpace against BufferTooSmall), which are the design's open decision.
- The FIXMEs and repair items the drops pointed to: `activate()` in `docs/repairs/261002/mm.md` item 2 and `docs/repairs/260908/heap.md` item 4, `unpark_preempt()` in `docs/repairs/260917/kernel-task-sched.md` item 2, the IOAPIC EOI in `docs/repairs/261003/x86-apic.md` item 1.

**How it was found**
Step 2 of `.agents/tasks/system-abi-surface/DESIGN.md`.

**The user's prediction**
"Many, the kernel far more than ring 3." Measured: 44, all in the kernel and none in ring 3. The direction held, but there were fewer than "many".

**Three questions you should be able to answer**
- Why does putting the attribute on the type cover functions written after it, when Zircon's per-function marking does not?
- Given a dropped Status, how do you choose between `static_cast<void>`, `DEBUG_ASSERT` and handling it?
- What does a `DEBUG_ASSERT` on a callee's answer buy in a release build, and what does it not?
