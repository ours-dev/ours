# echo changes

## Title: An echo client is the Loop's Waiter, and the static trampoline that cast its context back is gone (2026-10-29)

**Files**
- `examples/echo/echo_server.cpp` — `EchoServer::Client` is `final: public loop::Waiter` and its private `on_signals(observed)` hands the call to `EchoServer::serve()`; the static `EchoServer::on_signals(void *, Signals)` is gone; `connect()` fills the entry field by field and registers `client` itself; the comments that named the callback say `on_signals()`
- `docs/repairs/261029/echo.md` — new: a client's entry stays held when the Loop frees its slot after a failed re-arm
- `.agents/tasks/loop-waiter-echo/PLAN.md`, `RESULT.md` — this step's workspace

**Symptom**
Latent, cannot be triggered today: the server behaves as before. This is step 4 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, after `docs/changes/261029/loop.md` added `loop::Waiter`, `ioservice.md` moved `LoopServer::Binding` and `init.md` moved the root's connections. The echo example had a static function whose only work was to cast the slot's `void *context` back to its client and call `serve()` on the client's server:

```cpp
// before
static auto on_signals(void *const context, Signals const observed) -> loop::Action {
    auto &client = *static_cast<Client *>(context);
    return client.server->serve(client, observed);
}
// connect()
client = Client{.server = this, .channel = channel};
if (auto const added = m_loop->add(channel, Signals::Readable | Signals::PeerClosed,
                                   &EchoServer::on_signals, &client);
```

**Cause**
Why a client can be the Waiter: one client, one slot, one behaviour. `connect()` registers an entry once, for exactly one slot, and the slot's behaviour never changes: hand the observed signals to the server together with the client. The `void *` was the client already, and the client already knew its server, so the trampoline took an object's address as an untyped pointer in order to call a function that the object could have had. Making the client the `Waiter` removes that detour, and with it the one place where the compiler could not check that the pointer given to `add()` and the type cast back in the function were the same. (Contrast `Realm`, step 5: one `Instance` registers two waits with two behaviours, so it cannot be the Waiter of both.)

What the Loop is promised, and why `Storage` keeps it. `Waiter` says the object stays where it is from `add()` until its slot is freed. The clients are the entries of `EchoServer::Storage`, which `main()` builds once with `ECHO_STORAGE.init()` in a global and never moves or destroys. The worst interleaving is an entry released and taken again while the Loop still holds its address for the old life. The order that rules it out, all on the one thread that runs the Loop: `serve()` calls `release()`, which marks the entry free (`channel` Invalid) and answers `Close`; `run()` closes the handle, bumps the slot's generation and frees the slot (step 4 of `run()`) before it dispatches anything else; only a later dispatch can run `connect()`, from `Outgoing`'s own Waiter, and find the entry free. A packet of the old life still on the port names the old generation and is dropped by step 1 of `run()` before it reaches `on_signals()`. When `add()` fails in `connect()`, no slot exists: the entry is marked free again and nothing holds its address. One path breaks the order: a re-arm that fails frees the slot without the entry being released. It cannot happen today and is recorded in `docs/repairs/261029/echo.md`; it is the same defect as item 1 of `docs/repairs/261029/ioservice.md`.

Why `connect()` had to change. `Client{.server = this, .channel = channel}` is aggregate initialisation, and a class with a virtual function is not an aggregate, so it stopped compiling. The forms that remain:
- a constructor taking both fields, to keep the shape `client = Client{*this, channel};`. Rejected: it needs a second constructor beside the default one that `std::array` requires, and an assignment of a `Waiter`-derived temporary (the implicit copy assignment calls `Waiter`'s protected one, which a derived class may), for what two assignments do;
- two assignments to the entry's public fields, `client.server = this; client.channel = channel;`, chosen. It is how this file already treats an entry: `release()` and the failed `add()` write `channel` directly. It changes one line. No invariant needs a function to guard it: nothing runs between the two writes, and the Loop learns of the entry only at `add()`. The server is written first because the entry counts as in use once `channel` is set, which is what the loop over `clients` tests;
- a member function of `Client` that sets both. It would be the only verb among bare field writes, for the sake of two assignments;
- private fields with `m_` names and `friend class EchoServer`, as `Binding` and the root's connections have. Those classes already had private fields, so steps 2 and 3 kept them; here the fields were public in a private nested type, and changing that renames seven uses of `channel` and changes `serve()`'s text for no behaviour.

Why `on_signals()` is private and defined in the class. Only the Loop calls it, and it calls it through `Waiter`, where it is public. `Client` is a member of `EchoServer`, so its body may call `serve()`, which is private to the server, and a function body inside a nested class is compiled with the enclosing class complete, so `serve()` need not be declared before `Client`. The program is one translation unit, so there is no reason to move the definition out of the class to place the vtable in one object file, as `Binding` does.

What it saves and what it costs, measured. It saves the static function and the `static_cast`; it saves no machine code: the compiler had inlined `serve()` into the trampoline, and `EchoServer::on_signals(void *, Signals)` (579 bytes) is now `EchoServer::Client::on_signals(Signals)` (579 bytes). It costs a vptr per client. `sizeof`, from `-fdump-record-layouts-simple` on the compile command of `echo_server.cpp` in `compile_commands.json`:

| Object | Before | After |
|---|---|---|
| `EchoServer::Client` | 16 | 24 |
| `std::array<Client, 4>` | 64 | 96 |
| `EchoServer::Storage` | 8512 | 8544 |

The `Waiter` subobject is at offset 0, `server` at 8 and `channel` at 16. The program, from `llvm-size -A`, `llvm-nm -S -C` and `llvm-readelf -l` on `echo_server`, before and after:

| | Before | After |
|---|---|---|
| `.text` | 25646 | 25694 |
| `.data.rel.ro` | 368 | 392 |
| `.rela.dyn` | 576 | 600 |
| `.bss` | 22992 | 23024 |
| `.data` | none | none |
| `ECHO_STORAGE` | 8512, type `b` | 8544, type `b` |
| RW `LOAD` segment, file size | 0x2b0 | 0x2c8 |
| stripped image (`echo_server.image.elf`) | 29440 | 29552 |

The new `.data.rel.ro` bytes are the vtable of `Client`, 24 bytes (`vtable for EchoServer::Client`; the program has no RTTI, so it is two zero words and the one function), and its one relative relocation is the 24 bytes of `.rela.dyn`. The `.text` growth is 48 bytes, all in `main()` (466 to 516 bytes) except 6 bytes that `connect()` lost (319 to 313): `ECHO_STORAGE.init()` now has to construct the four clients, so `main()` stores the vptr into each. That is the whole run-time cost of the change outside the dispatch.

**Why nothing moved from `.bss` to `.data`**, which is what step 3 did to the root's resource storages (`docs/changes/261029/init.md`, `docs/repairs/261029/init.md` item 1). A global lies in `.bss` only if every byte of its initial value is zero, and a `constinit` object that holds a `Waiter` has a vptr in its initial value, so the whole object, buffers included, goes into the file. `ECHO_STORAGE` is not such an object: it is `constinit ustl::LazyInit<EchoServer::Storage>`, which is raw storage (`alignas(T) std::byte m_storage[sizeof(T)]{}`), all zeros, and the vptrs are stored at run time by `init()`, in `main()`. `llvm-nm -S` lists `ECHO_STORAGE` as `b` before and after, and `llvm-size -A` shows no `.data` section either way. This is also the reason `LazyInit` is there (the comment above it in the file): no dynamic initialiser runs in a user program, and no heap is needed; `init()` is an explicit call.

Until step 6 a call to a client costs the extra indirect branch that `docs/changes/261029/loop.md` describes: `run()` calls `call_waiter()`, which makes the virtual call. A client's work per message is a `channel_read` and a `channel_write`, each a system call.

**Change**
```cpp
// after
class Client final: public loop::Waiter {
  public:
    EchoServer *server = nullptr;
    Uandle channel = Uandle::Invalid;

  private:
    auto on_signals(Signals const observed) -> loop::Action override {
        return server->serve(*this, observed);
    }
};
// connect()
client.server = this;
client.channel = channel;
if (auto const added =
        m_loop->add(channel, Signals::Readable | Signals::PeerClosed, client);
```
The comments that named the callback, the context or the slot's callback now name `on_signals()`: the description of `EchoServer`, `Storage`'s buffers ("one `on_signals()` runs at a time"), `stop()`'s account of exiting from inside one, and the comment in `main()`. `Client`'s comment says what a client is to the Loop and why the Loop's keeping its address is safe. `BUILD.gn` and every other file are unchanged.

**Reference**
None, this project's own design. It follows `docs/changes/261029/loop.md`, `ioservice.md` and `init.md`, and the abstract classes of 2026-10-28 (`ioservice::LoopServer::Delegate`, `launch::LauncherOps`).

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build) succeeds. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `.../ioservice/test/outgoing` 9, `.../ioservice/test/loop_server` 13, `host/obj/system/service/init/test/autorun` 5; `boot_check.py` runs all 148 host tests of `//:default` and passes them.
- No host test is added: echo calls `channel_read`, `channel_write` and `process_exit` itself, and the host fake covers `ioservice` and `loop` only. On the target `routing_checks` exercises it through `ours.test.Echo`: an `EchoString` routed through a sibling's `expose`, and `Stop`, which ends the program. The rel image logs `init: started echo`, then `init: echo exited with 0` and `routing_checks: passed`, and `boot_check.py` requires both of the last two.
- A deliberate breakage, the source put back after (`cmp` against a saved copy): `Client::on_signals()` answering `Close` at once, without calling `serve()`, and the rel image booted. `routing_checks` reported `EchoString through the sibling's expose = PeerClosed (expected Ok) FAILED` and `Stop = PeerClosed (expected Ok) FAILED`, then `2 check(s) FAILED` and `init: routing_checks exited with 1`; `boot_check.py` printed `rel FAIL`, 4/6 required lines, the missing ones being `routing_checks: passed` and `init: echo exited with 0`. So the new path is on the boot path and the boot depends on it. The checks fail with `PeerClosed` because the Loop closes the channel with the request unread.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/examples/echo/.*'`, on `echo_server.cpp`: exit 0, no diagnostic. `scripts/format_cpp.py --check`: clean. `scripts/lint_docs.py --changed` on each document: no violations.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-echo` in `.worktrees/lw-echo` with the final tree, a build directory configured by the run: build ok in 7.0 s; dbg PASS (shell finished at 5.7 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `routing_checks: passed`, `init: echo exited with 0` and `pc-ps2: key a` and `pc-ps2: key A`; both logs hold `shell: loop checks passed`. This tree holds steps 1 to 4 together, which had not been booted as one tree before.
- Not measured: the time of a message through a client before and after, and anything on x86-64 hardware.

**Deliberately not done**
- The clients' fields stay public in the private nested type, and no `friend` or `m_` renaming is done; see the three forms in the Cause.
- No deleted copy on `Client`: the old struct was copyable and nothing copies a `Client` or a `Storage`. Its implicit copy stays accessible, since `Waiter`'s copy is protected and not deleted (`docs/changes/261029/loop.md`).
- No `static_assert` on the size of a client or of `Storage`: it is a cost, not an invariant.
- The failed-re-arm path that leaves an entry held is not fixed; it needs a decision on the Loop's spec (`docs/repairs/261029/echo.md`).
- `Realm`, the shell's `check_loop()` and the removal of `Callback` are steps 5 and 6. `docs/guide/` quotes this file by line and is another session's; `RESULT.md` lists which lines moved.

**How it was found**
None. The absence of a `.bss` to `.data` move was checked by measuring `llvm-nm -S` and `llvm-size -A` on the echo program before and after, because step 3 had found that move in the root.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The Loop keeps a client's address from `add()` until its slot is freed. In which order do `release()`, step 4 of `run()` and `connect()` run so that a taken-again entry is never reached through the old registration, and which path leaves an entry held for good?
2. `client = Client{.server = this, .channel = channel};` stopped compiling. What about `Waiter` makes `Client` no aggregate, and what would be wrong with overwriting an object that the Loop holds the address of, were the entry not known to be free?
3. Every echo client now has a vptr, yet `ECHO_STORAGE` stayed in `.bss`, while the root's five `ResourceServer::Storage` globals moved to `.data` in step 3. What differs between the two, and where does the echo program store its vptrs?
