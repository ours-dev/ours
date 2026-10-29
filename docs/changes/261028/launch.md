# launch changes

## Title: launch::LauncherOps is an abstract class a connection derives from, in place of a struct of function pointers (2026-10-28)

**Files**
- `system/lib/launch/include/launch/process_launcher.hpp`, `system/lib/launch/src/process_launcher.cpp` — `LauncherOps` is a class with pure virtual `start(startup)` and `close(handle)` and a protected, defaulted destructor; `ProcessLauncherState` takes a `LauncherOps &` and keeps its address in place of a copy of the struct; the two calls are `m_ops->start(startup)` and `m_ops->close(handle)`
- `system/service/init/launcher_server.hpp`, `system/service/init/launcher_server.cpp` — `LauncherConnection` derives privately from `LauncherOps` and overrides both; its static `start()` and `close()` that took a `void *context` and cast it back are gone, and its constructor is `m_state(*this)`
- `system/lib/launch/test/process_launcher_test.cpp` — the `Fake` derives from `LauncherOps` and its two functions are members; the three states are given `fake` directly
- `system/lib/launch/README.md` — names the class

**Symptom**
Latent, cannot be triggered today. The first of the five function-pointer-and-context structs left in system/ (listed for the user on 2026-10-28), picked because it is the clean case: one program implements it, the other implementer is the test's fake, and it is called once per launched process. `LauncherConnection` had two static functions whose only work was to cast a `void *` back, and the connection's `m_server` was reached through one of them.

**Cause**
`LauncherOps` was written as a pair of function pointers and a context for the reason `elfload::Diagnostics` is one, that a library should make no system call and still be testable on the host. That reason is about having a seam, not about its shape; an abstract class is the same seam, with the `void *` turned into the object itself.

**Change**
```cpp
// before
constexpr LauncherConnection()
    : m_state(launch::LauncherOps{.start = &LauncherConnection::start,
                                  .close = &LauncherConnection::close, .context = this}) {}
static auto start(void *context, launch::Startup const &startup) -> ...;   // casts context back

// after
class LauncherConnection final: private launch::LauncherOps {
    constexpr LauncherConnection() : m_state(*this) {}
    auto start(launch::Startup const &startup) -> ... override;            // is the connection
```
The pool of connections is a `constinit` global that nothing at run time initialises, so the state has to point at its `LauncherOps` at compile time: the connection is its own `LauncherOps`, whose address is a constant there, and its vtable pointer is a constant too. `constinit` is the compiler's check on that, and the test keeps its own, `CONSTANT_STATE`. `LauncherOps`'s constructor is `constexpr` so a derived class can be constant-initialised; its destructor is protected and not virtual, for the reason `Delegate`'s is (`docs/changes/261028/ioservice.md`).

**Reference**
None, this project's own design.

**Verification**
- `host/obj/system/lib/launch/test/process_launcher`: 15 passed, with `constinit ProcessLauncherState CONSTANT_STATE{CONSTANT_FAKE}` still compiling; `.../plan`: 36 passed; `.../program`: 8 passed.
- A deliberate breakage of `process_launcher.cpp`, `close` never called through the ops, was caught by 14 of the 15 tests; the source was put back. A second, in `start()`, did not build (unused variable) and was not retried: every launch test checks what `start()` was handed.
- clang-tidy on `process_launcher.cpp`, `process_launcher_test.cpp` and `launcher_server.cpp` with the header filter on `system/lib/launch` and `system/service/init`: no diagnostic. clang-format: clean. The full build, init included, links.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change (`.worktrees/refactor-launcher-ops`, at HEAD): build ok in 9.0 s; dbg PASS, shell finished at 5.9 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. rel.log holds `routing_checks: passed`, which includes its checks of the Launcher: it connects to `ours.process.Launcher` and asks for launches the Launcher must refuse, so the real `LauncherServer` served them through connections of the constinit pool, each with its vtable pointer. That is the check that matters here.

**Deliberately not done**
- `loop::Callback`, `elfload::ImageWriter`, `elfload::Diagnostics` and `ioservice::Syscalls` are not touched; the first two are separate decisions, the last two are left as they are.

**How it was found**
The user chose it from the list, "先做 1".

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must `LauncherConnection`'s constructor be `constexpr`, and what in the pool's declaration makes the compiler check it?
2. A `constinit` object with virtual functions holds a vtable pointer. What has to be true of the loader for that pointer to be right at run time, and what already shows it is?
3. `ProcessLauncherState` kept a copy of the struct and now keeps a pointer. What must the program guarantee about the `LauncherOps` that it did not have to before?
