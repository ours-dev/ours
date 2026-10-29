# ioservice changes

## Title: LoopServer calls back into a Delegate, an abstract class, instead of a struct of function pointers and a void * (2026-10-28)

**Files**
- `system/lib/ioservice/include/ioservice/loop_server.hpp`, `system/lib/ioservice/src/loop_server.cpp` — `ioservice::Handlers` is replaced by `LoopServer::Delegate`, with `connect()` pure virtual and `write_failed()` virtual; the constructor takes a `Delegate *` and `set_handlers()` is `set_delegate()`
- `system/lib/svc/include/svc/outgoing.hpp`, `system/lib/svc/src/outgoing.cpp` — `Outgoing` derives privately from `Delegate` and overrides `connect()`, and its static `on_connect()` that only forwarded to the member is gone
- `system/lib/ioservice/test/loop_server_test.cpp` — the recorder is a `Delegate` subclass in place of two free functions
- `system/lib/ioservice/README.md`, `docs/repairs/261004/svc.md` — name the Delegate

**Symptom**
Latent, cannot be triggered today. `Handlers` was a hand-written interface: a struct of function pointers plus a `void *context`. The user asked why it was not an abstract class. The cost showed in `svc::Outgoing`, which needed a static function taking a `void *` whose only job was to cast it back and call a member. system/ has seven such pairs, `loop::Callback`, `elfload`'s two, `launch`'s `LauncherOps`, `svc::Handler` and `BootServer::Connector` among them; this change converts the one that is new.

**Cause**
The pairs follow each other: the first (`loop::Callback`) was written before a class could say the same thing without a heap, and each later one copied the shape. `void *` has no type check, so a wrong `context` compiles and fails at run time, and the forwarding function is boilerplate the compiler could write. I had not checked whether a virtual call was possible in a user program, and said wrongly that nothing in system/ used `virtual`: my search had excluded `eftl`, whose `BasicImage` is a class with virtual functions that init links. `system/lib/ort/src/cpp_dependencies.cpp` already defines `__cxa_pure_virtual` and `operator delete` for exactly that reason.

**Change**
```cpp
// before
struct Handlers {
    auto (*connect)(void *context, u64 service, ours::Uandle channel) -> void = nullptr;
    auto (*write_failed)(void *context, ours::Status status) -> void = nullptr;
    void *context = nullptr;
};
// Outgoing: ioservice::Handlers{.connect = &Outgoing::on_connect, .context = this}
//           static on_connect(void *context, ...) { static_cast<Outgoing *>(context)->connect(...); }

// after
class Delegate {
  public:
    virtual auto connect(u64 service, ours::Uandle channel) -> void = 0;
    virtual auto write_failed(ours::Status status) -> void;   // does nothing unless overridden
  protected:
    ~Delegate() = default;
};
// Outgoing: class Outgoing final: private ioservice::LoopServer::Delegate, passes `this`
```

The destructor is protected and not virtual. Nothing is deleted through a `Delegate`, and a virtual destructor would add a deleting destructor, which calls `operator delete` in a program with no heap (the refusing one in `ort`). `Outgoing` derives privately: the base is an implementation detail, and a caller of `Outgoing` cannot treat it as a `Delegate`. `write_failed()` keeps an out-of-line empty body in `loop_server.cpp`, which makes that file the one that emits `Delegate`'s vtable.

Rejected: a class template on the delegate type, which would need no `virtual` and could inline, but puts the whole implementation in a header and gives the host test a second instantiation; keeping function pointers, which matches the other six.

`svc::Handler`, the per-protocol function pointer the program gives `add_protocol()`, is public and used by echo; it keeps its shape for now, and `BootServer::Connector` goes when `BootServer` moves onto `LoopServer`.

**Reference**
None, this project's own design. The approach follows `eftl`'s `BasicImage` in being the project's one earlier use of virtual functions in a user program.

**Verification**
- `host/obj/system/lib/ioservice/test/loop_server`: 12 passed; `.../svc/test/svc`: 9 passed, `outgoing_test.cpp` untouched; `.../ioservice/test/ioservice`: 42 passed.
- Two deliberate breakages of `loop_server.cpp`, each caught: the delegate not told a reply failed (`ReplyThatCannotBeWrittenIsReportedAndEndsTheConnection`), an Open of a service node not reaching the delegate (`OpenOfAServiceReachesTheConnector` and `OpenOfADirectoryMakesAConnectionOfItsOwn`). The source was put back.
- clang-tidy on `loop_server.cpp`, `outgoing.cpp` and `loop_server_test.cpp` with the header filter on both libraries: no diagnostic. clang-format and gn format: clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in a clean worktree holding only this change: `.worktrees/refactor-delegate`, at HEAD: build ok in 7.6 s; dbg PASS, shell finished at 5.6 s, 44 passed, 4 skipped, 0 failed; rel PASS, shell finished at 4.6 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines; host tests PASS, 148 test programs passed, 0 failed. rel.log holds `routing_checks: passed` and `init: echo exited with 0`. It is the check that matters here, since echo is a user program that now has a vtable of its own through `Outgoing`: it links and serves.

**Deliberately not done**
- `svc::Handler`, `BootServer::Connector`, `loop::Callback`, `launch::LauncherOps` and `elfload`'s two keep their shape. Converting them is a separate decision for each: some are called on hot paths, and `loop::Callback` is one function per slot, not an object.
- No virtual destructor, no RTTI use, no `dynamic_cast`.

**How it was found**
The user read `Handlers` in `loop_server.hpp` and asked why it was not an abstract class.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is `Delegate`'s destructor protected and not virtual, and what would a virtual one have cost a program with no heap?
2. `Outgoing` passes `this` to the `LoopServer` while its own constructor is still running. When would that be unsafe with a virtual `connect()` that it would not be with a function pointer?
3. Where is `Delegate`'s vtable emitted, and why does that depend on `write_failed()` having an out-of-line definition?
