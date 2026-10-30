# init changes

## Title: The root's built-ins are one generated list, and a protocol mismatch is logged with both protocols (2026-10-29)

**Files**
- `system/service/init/builtins.json` — new: the root's seven built-ins, in the order `BUILTINS` had them, the one list the manifest compiler and the root both read
- `system/service/init/BUILD.gn` — the action `:builtins.gen` runs `//system/component_builtins.py` on the file and writes `builtins.inc` under `$target_gen_dir`; `:builtins` is the group that carries the action and the include path to `:init`
- `system/service/init/realm.cpp` — `BUILTINS` is defined by `#include "builtins.inc"` where it was written out; `has_protocol_builtin()` and six `static_assert`s tie each name a library defines to the list; `log_route_error()` has a line of its own for `BadType`
- `kernel/products/image/dev/BUILD.gn` — `root.cm` gains `builtins = "//system/service/init/builtins.json"`, so the build check also resolves a route that ends at a built-in
- `docs/repairs/261028/init.md` — item 1 rewritten for what remains
- `docs/repairs/261029/ours-verify.md` — new: a tooling hazard found along the way

**Symptom**
Two things were missing after the run-time check (`docs/changes/261029/component.md`) and the build check (`docs/changes/261029/component-manifest.md`) were in.

First, the build check could not see a route that ends at a built-in. The root's built-ins were a C++ list in `realm.cpp` and nothing the compiler could read, so an offer of the Launcher under the name `ours.resource.Irq` built without a word. This is the experiment of 2026-10-29, which `root.cm` could not even be given to check.

Second, the root's log said of a mismatch that the use "routes nowhere", with no protocol named, while the route had been found. What the experiment showed before the checks existed:

```text
init: started pc-ps2
pc-ps2: getting the IRQ resource: PeerClosed
init: pc-ps2 exited with 1
```

**Cause**
Two lists described one thing. `BUILTINS` was written out in `realm.cpp` with each name taken from a library constant, and the compiler, which runs on the host before an image exists, could not include a C++ header. A second list kept by hand in the manifest compiler's inputs would have agreed with the first only until someone added a protocol to one of them; the build check would then have been silent about exactly the routes it was written to check, and silence looks like a pass. So the list moves to a data file both sides read, and the C++ side is generated from it: the file is the only place a built-in is written down.

The log line was the second half. `log_route_error()` printed one shape for every failure, `routes nowhere: <Status> at <where> (<verb> of "<name>")`, which fits a walk that stopped. A `BadType` is the opposite: the walk reached a capability and the capability is of another protocol. The words "routes nowhere" are false of it, and the verb and name it printed (`declaration of "ours.process.Launcher"`) name a declaration the person reading the log has no reason to connect with the use. The information the person needs, which two protocols were mixed up, is in `RouteError::want` and `got` since the run-time check, and was not printed.

**Change**
`builtins.json`, with the names checked against the constants the old list used (`ours_process::LAUNCHER_NAME`, `ours_resource::*_PROTOCOL.name`) and each type left out, so it is the name:

```json
{
  "builtins": [
    {"name": "boot", "kind": "directory", "rights": ["rx*"]},
    {"name": "ours.process.Launcher", "kind": "protocol"},
    {"name": "ours.resource.Debug", "kind": "protocol"},
    {"name": "ours.resource.Sampling", "kind": "protocol"},
    {"name": "ours.resource.Irq", "kind": "protocol"},
    {"name": "ours.resource.Ioport", "kind": "protocol"},
    {"name": "ours.kernel.RootJob", "kind": "protocol"}
  ]
}
```

`realm.cpp`, before and after (the seven `component::Builtin{...}` entries are what `component_builtins.py` writes, in `out/<dir>/user-x86/gen/system/service/init/builtins.inc`):

```cpp
// before
constexpr auto BUILTINS = std::array{
    component::Builtin{
        .kind = Kind::Directory,
        .name = "boot",
        .rights = ustl::Option<component::Rights>(component::Rights::RxStarDir),
    },
    component::Builtin{
        .kind = Kind::Protocol,
        .name = ours_process::LAUNCHER_NAME,
        .rights = ustl::NONE,
    },
    // ... five more
};

// after
#include "builtins.inc"

constexpr auto has_protocol_builtin(std::string_view const name) -> bool {
    for (auto const &builtin: BUILTINS) {
        if (builtin.kind == Kind::Protocol && builtin.name == name) {
            return true;
        }
    }
    return false;
}

static_assert(has_protocol_builtin(ours_process::LAUNCHER_NAME));
static_assert(has_protocol_builtin(ours_resource::DEBUG_PROTOCOL.name));
// ... four more
```

The generated entries carry `.type`, which the hand-written ones left out; for a built-in it is its name, so `route()` compares exactly what it compared before for a use that does not rename. The `static_assert`s are the half of the drift the library constants can still cause: the names the old list took from the libraries are now strings in a JSON file, and a library that renames its protocol, or a list that drops a name, stops the build here instead of at a boot where the route ends at nothing. They look a name up in the list; they do not and cannot say that `connect()` has a branch for it, which is what stays in `docs/repairs/261028/init.md` item 1.

`log_route_error()` keeps its line for every status but `BadType`, which gets:

```cpp
if (error.status == Status::BadType) {
    auto provided_by = std::string_view{};
    if (error.at != nullptr) {
        provided_by = "provided by ";
    }
    auto _ = ort::debug_line({"init: ", moniker, "'s use of \"", use.source_name,
                              "\" reaches another protocol: it wants ", error.want,
                              ", the route ends at ", error.got, " (", provided_by, at, ")"});
    return;
}
```

`at` is `the built-ins` when `error.at` is `nullptr`, and the moniker of the providing instance otherwise, as the existing line already had it. Every piece is a `std::string_view` on a string literal, on the manifests' bytes, or on the moniker's stack buffer: the route path allocates nothing before this change and nothing after. The epitaph needed no code: `Realm::connect()` closes the channel with `routed.unwrap_err().status` and that is `BadType`, which the experiment below shows reaching the client.

Rejected: a hand-written second list for the compiler, kept equal to `realm.cpp` by hand (decision 3 (b) of the design): it agrees until the first protocol is added to one of them. Rejected: naming the generated file `builtins.hpp`; it defines a `constexpr` that has to sit inside the anonymous namespace `realm.cpp` already has, and `.inc` says that it is not a header to include anywhere else.

**Reference**
None, this project's own design. Fuchsia's component_manager registers its built-in protocols in code, in `BuiltinEnvironment::new()` with `add_builtin_protocol_if_enabled()` (`src/sys/component_manager/src/builtin_environment.rs:596-597`, `:634-648`, as read in `docs/changes/261005/init.md`); ours keeps them in a data file because two programs, the root and the manifest compiler, need the same list (`docs/design/261029/protocol-identity.md`, decision 3).

**Verification**
- The experiment of 2026-10-29, again, in a scratch worktree (`.worktrees/pi-root-experiment`, started from the commit before this one with this change applied, never committed, deleted afterwards). Its `root.cm` offers the Launcher to `#pc-ps2` as `ours.resource.Irq` in place of the offer of the Irq root.

  (a) With the check on, the build of `root.cm` fails, with the text of `DESIGN.md`, "Error output", item 1:

  ```text
  FAILED: [code=1] gen/kernel/products/image/dev/root.cm
  python3 ../../system/component_manifest.py --source gen/kernel/products/image/dev/root.cm.source.json --program gen/kernel/products/image/dev/root.cm.program.json --children gen/kernel/products/image/dev/root.cm.children.json --tree gen/kernel/products/image/dev/root.cm.tree.json --output gen/kernel/products/image/dev/root.cm --depfile gen/kernel/products/image/dev/root.cm.d --builtins ../../system/service/init/builtins.json
  error: //kernel/products/image/dev:root.cm: #pc-ps2 uses protocol "ours.resource.Irq" (type ours.resource.Irq),
         but the route ends at the root's built-in "ours.process.Launcher" (type ours.process.Launcher):
           use    #pc-ps2 "ours.resource.Irq"
           offer  root.cm "ours.process.Launcher" from parent to #pc-ps2 as "ours.resource.Irq"
         Offer a capability of type "ours.resource.Irq" under that name, or give the use the type it means.
  ninja: build stopped: subcommand failed.
  ```

  (b) With `type_check = false` on `root.cm` the image builds and boots (`boot_check.py --only rel --no-tests`; it reports FAIL for the three `pc-ps2:` lines that never come, which is the point). The rel log, `init:` and `pc-ps2:` lines in order, with the shell's interleaved lines left out:

  ```text
  init: started pc-ps2
  init: pc-ps2's use of "ours.resource.Irq" reaches another protocol: it wants ours.resource.Irq, the route ends at ours.process.Launcher (the built-ins)
  pc-ps2: getting the IRQ resource: BadType
  init: pc-ps2 exited with 1
  ```

  Where it read `PeerClosed` and nothing from the root, pc-ps2 now reads `BadType`, the status of the epitaph `connect()` wrote, and the root says which two protocols were mixed up. `routing_checks: passed` was still logged, so the one mismatch broke only the component that had it.

  (c) The other branch of the line, a provider and not a built-in: with `type_check = false` and routing_checks' `type = "ours.examples.echo.Echo"` changed to `"ours.examples.echo.EchoWrong"`:

  ```text
  init: routing_checks's use of "ours.test.Echo" reaches another protocol: it wants ours.examples.echo.EchoWrong, the route ends at ours.examples.echo.Echo (provided by echo)
  routing_checks: EchoString through the sibling's expose = BadType (expected Ok) FAILED
  ```

- What each of the three audiences sees, from the same mismatch. The person building: the build fails and prints the route and the fix (a); no image exists. The person reading the root's log, when the check was off or the image was not built from these manifests: the line of (b) or (c) names the use, both protocols and where the route ended. The component that asked: its channel closes with the epitaph `BadType`, and `ours_resource::get()` returns it (`system/lib/ours.resource/src/client.cpp:68-81`), so the component's own report reads `BadType`; a component that does not read epitaphs still sees PeerClosed, as for any refused Open.
- `grep -n "component::Builtin{" system/service/init/realm.cpp` finds nothing: there is no hand-written list.
- The `static_assert`s, by edit and revert: a seventh `static_assert(has_protocol_builtin("ours.not.InTheList"))` fails the compile of `realm.cpp` with `static assertion failed due to requirement 'has_protocol_builtin("ours.not.InTheList")'`. The edit was reverted; `grep -c InTheList system/service/init/realm.cpp` is 0.
- `python3 system/test_component_manifest.py`: 99 tests OK, with `OURS_BUILD_DIR` set so that the cross-check over the fixtures runs. `python3 system/test_component_builtins.py`: 26 tests OK. `decl` 20 tests passed and `route` 41 tests passed, run from `out/bringup.x86`.
- clang-tidy against `out/bringup.x86` on `system/service/init/realm.cpp`: exit 0, no diagnostic. `scripts/format_cpp.py` reformatted the lines of this change (the range-for colon); `scripts/format_gn.py --check` is clean on both `BUILD.gn` files.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in `.worktrees/pi-root` with the final tree: build ok; dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines; rel PASS, 41 passed, 8 skipped, 0 failed, 6/6 required lines (`routing_checks: passed` among them), 0 error lines; host tests PASS, 148 passed, 0 failed. `root.cm` is compiled with `--builtins` and its check found no mismatch in the real image, so the manifests needed no `type` beyond the one `routing_checks` already states. The rel log has the two deliberate `routes nowhere` lines of routing_checks (`denied`, `ours.test.Unoffered`) and no `reaches another protocol` line.

**Deliberately not done**
- The id and the `switch` that would let `connect()` stop comparing names: `docs/repairs/261028/init.md` item 1, rewritten for what remains.
- A `type` on any built-in. All seven are their own protocol, so the default (the name) is right; the field is in the data file's format for the first built-in that needs one.
- `Builtin`'s comment in `route.hpp` still says "the root holds the list". It does, generated; the description of the data file in the library's README and the design documents is the plan's step 5.
- The line for a mismatch does not print the offer or expose the route took. The build error does; at run time the root has only the end of the walk (`RouteError::at`, `name`), and adding the hops would mean recording them in `route()` for the one error that wants them.

**How it was found**
None. It is the fourth and last implementation step of the plan accepted on 2026-10-29. The scratch experiment was the evidence the plan asked for.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does the manifest compiler read a JSON file and `realm.cpp` include a file generated from the same JSON, when the compiler could have been given a copy of the list by hand? What does the build check silently stop doing, and how would anyone notice, if the two lists are written separately and one drifts?
2. The `static_assert`s ask whether the list holds each library's protocol name. Which drift do they stop, and which drift between the list and `Realm::connect()` do they not stop?
3. In the experiment, why did the build fail with the check on, while with it off pc-ps2 still read `BadType` and not `PeerClosed`? Which two pieces of code produced each?

## Title: The root's Resource and Launcher connections are the Loop's Waiters, and the two static trampolines go (2026-10-29)

**Files**
- `system/service/init/resource_server.hpp`, `system/service/init/resource_server.cpp` — `ResourceConnection` is `final: public loop::Waiter` and its private `on_signals(observed)` hands the call to `ResourceServer::handle()`; `ResourceServer` befriends it; the static `ResourceServer::on_signals(void *, Signals)` is gone; `bind()` registers `connection` itself
- `system/service/init/launcher_server.hpp`, `system/service/init/launcher_server.cpp` — the same for `LauncherConnection`, which derives `private launch::LauncherOps, public loop::Waiter`; the static `LauncherServer::on_signals(void *, Signals)` is gone
- `docs/repairs/261029/init.md` — new: the resource servers' pools are now written into init's image
- `.agents/tasks/loop-waiter-initservers/PLAN.md`, `RESULT.md` — this step's workspace

**Symptom**
Latent, cannot be triggered today: the servers behave as before. This is step 3 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, after `docs/changes/261029/loop.md` added `loop::Waiter`. Each server had a static function whose only work was to cast the slot's `void *context` back to its connection and call `handle()` on the connection's server:

```cpp
// before, in both servers
auto ResourceServer::on_signals(void *const context, Signals const observed) -> loop::Action {
    auto &connection = *static_cast<ResourceConnection *>(context);
    return connection.m_server->handle(connection, observed);
}
// bind()
m_loop->add(channel, CONNECTION_SIGNALS, &ResourceServer::on_signals, &connection);
```

**Cause**
Why a connection can be the Waiter: one connection, one slot, one behaviour. A connection is registered once, in `bind()`, for exactly one slot, and the slot's behaviour is always the same: hand the observed signals to the connection's server together with the connection. The `void *` was the connection already, and the connection already knew its server (`m_server`, set by `bind()`), so the trampoline was a function that took an object's address as an untyped pointer in order to call a function that the object could have had. Making the connection the `Waiter` removes that detour: the type the Loop calls is the type that holds the state. (Contrast `Realm`, step 5: one `Instance` registers two waits with two behaviours, so it cannot be the Waiter of both.)

What the Loop is promised, and why the pools keep it. `Waiter` says the object must stay where it is from `add()` until its slot is freed. A connection is an entry of a `constinit` global pool that never moves and is never destroyed, so the address is stable for the process's life. The worst interleaving is an entry rebound while the Loop still holds its address for the old life. The root runs the Loop and `bind()` on one thread, so the order is fixed: `handle()` marks the entry free (`m_channel = Invalid`) and answers `Close`; `run()` closes the handle, bumps the slot's generation and frees the slot (step 4 of `run()`); only then can another callback, `Realm::connect()` for instance, call `bind()` and take the entry again. A packet still queued from the old life names the old generation and is dropped by step 1 of `run()` before it reaches `on_signals()`. And when `add()` fails in `bind()`, no slot exists: the entry is marked free again and nothing holds its address.

Why `on_signals()` is private and defined in the `.cpp`. `ResourceServer` is only declared where `ResourceConnection` is defined, and `handle()` is private to the server, so the body needs the complete type and an access path: `ResourceServer` gets `friend class ResourceConnection;`, as `LauncherServer` already had for `LauncherConnection` (its `start()` calls the server's private `start()`). The override is private for the reason the connection's fields are: nothing but the Loop calls it, and the Loop calls it through `Waiter`, where it is public. `bind()` converts the entry to `Waiter &` at the call to `add()`, which needs `Waiter` to be a public base.

Why the Launcher's connection may take a second base. `LauncherOps` (`start`, `close`) and `Waiter` (`on_signals`) are abstract, hold no data and share no names, so deriving from both is not ambiguous. `LauncherOps` stays private, since only the state reaches it; `Waiter` is public. `constexpr LauncherConnection() : m_state(*this)` did not change: `*this` converts to `LauncherOps &` inside the class, and both vptrs are constants, so the `constinit` pool is still constant-initialised (it compiles under `constinit`, and the link finds no `__cxa_atexit`). The layout, from `-fdump-record-layouts-simple`: `LauncherOps` at offset 0, `Waiter` at 8, then `m_server` at 16, `m_channel` at 24 and `m_state` at 32. The two addresses differ, which is why the registration must pass the entry converted to `Waiter &` and not its address as a `void *`: stored as `void *` and cast back to `Waiter *`, the launcher's entry would have called `LauncherOps`'s first virtual function, `start()`, with a `Waiter`'s arguments. The old form never had this trap because it cast to the connection's own type; the `Waiter &` overload moves the adjustment to the compiler, at the call to `add()`. For the second base the compiler also emits a 23-byte `this`-adjusting thunk (`non-virtual thunk to init::LauncherConnection::on_signals`, `-0x8` on `this`) so that the `Waiter` subobject's vtable can reach the one override.

What it saves and what it costs, measured:
- Saves two static functions and the two `static_cast`s from `void *`. It saves no machine code: the trampoline of each server (19 bytes and 18 bytes) became the connection's `on_signals()` (19 and 19 bytes), and the Launcher gained the 23-byte thunk, so `.text` grew by 32 bytes.
- Costs a vptr per connection: `ResourceConnection` 16 to 24 bytes, `LauncherConnection` 5112 to 5120 bytes, which is two vptrs now where it had one. Over the fixed pools, from `sizeof` (a compile check in a scratch file with `template <usize N> struct Show;`, whose error prints each size):

| Object | Before | After | Pool |
|---|---|---|---|
| `ResourceConnection` | 16 | 24 | 4 per server |
| `LauncherConnection` | 5112 | 5120 | 4 |
| `ResourceServer::Storage` | 4448 | 4480 | 5 servers |
| `LauncherServer::Storage` | 28928 | 28960 | 1 |

The sum is 5 × 32 + 32 = 192 bytes of vptrs. That is the small cost the plan expected. The large one it did not: **each `ResourceServer::Storage` was all zeros and lay in `.bss`; a non-zero vptr makes it a `.data` object, whole, buffers included.** From `llvm-size -A` and `llvm-readelf -l` on `init`, before and after:

| | Before | After |
|---|---|---|
| `.data` | 28928 | 51360 |
| `.bss` | 77664 | 55424 |
| `.text` | 108766 | 108798 |
| RW `LOAD` segment, file size | 0x7100 | 0xc8a0 |

The five resource storages (4448 bytes each, `llvm-nm -S`: type `b` before, `d` after) moved 22,240 bytes out of `.bss`, and init's file grows by about 22 KiB, nearly all zeros: the RW segment's file size by 22,432 bytes, and the whole file, stripped alike with `llvm-objcopy --strip-all`, from 154,384 to 177,544 bytes. It is the same cost `docs/repairs/261004/init.md` item 9 records for the Launcher's pool, now five times over. It is recorded as a repair item of its own, `docs/repairs/261029/init.md`, with the options; it is not fixed here, since moving the buffers out of `Storage` changes `init.cpp` and what a server owns, which this step does not touch.

Until step 6 a call to a connection costs the extra indirect branch that `docs/changes/261029/loop.md` describes: `run()` calls `call_waiter()`, which makes the virtual call. For the Launcher it is one `this`-adjusting thunk more. A server's work per message is a `channel_read`, a `channel_write` and, for a Launch, `launch::start()`; the branch is a small part of one system call.

**Change**
```cpp
// resource_server.hpp
class ResourceConnection final: public loop::Waiter {
  public:
    constexpr ResourceConnection() = default;

  private:
    friend class ResourceServer;

    auto on_signals(ours::Signals observed) -> loop::Action override;
    ResourceServer *m_server = nullptr;
    ours::Uandle m_channel = ours::Uandle::Invalid;
};
// in ResourceServer, private:
friend class ResourceConnection;

// resource_server.cpp
auto ResourceConnection::on_signals(Signals const observed) -> loop::Action {
    return m_server->handle(*this, observed);
}
// bind()
auto const added = m_loop->add(channel, CONNECTION_SIGNALS, connection);

// launcher_server.hpp
class LauncherConnection final: private launch::LauncherOps, public loop::Waiter {
    // on_signals() beside start() and close(), all three private overrides
};
```
The comments that named the callback or the context now name `on_signals()`: the two classes' descriptions, `Storage`'s buffers, "one message per on_signals()". `realm.*`, `boot_server.*`, `init.cpp`, `pools.hpp` and the BUILD file did not change. Rejected: a connection `on_signals()` that is public, which would let anything that holds an entry skip the server's `handle()` access; a `friend` of `Loop` in place of `Waiter`, which is what the abstract class replaces.

**Reference**
None, this project's own design. It follows the abstract classes of 2026-10-28, `ioservice::LoopServer::Delegate` and `launch::LauncherOps`, and `docs/changes/261029/loop.md`.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build) succeeds. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 12, `host/obj/system/service/init/test/autorun` 5; `boot_check.py` runs all 148 host tests of `//:default` and passes them.
- No host test is added: `ResourceServer` and `LauncherServer` call `channel_read`, `channel_write`, `handle_duplicate` and `launch::start()` themselves, and the host has a fake of the kernel only for `ioservice` and `loop`, so a test of them would need a new fake and is not a few lines. They are covered on the target by `routing_checks` (`Get` on the Debug, Irq, Ioport and Sampling protocols and three refused Launches, each on a connection of its own), the shell (the rel image runs every command through the Launcher, and its root job check goes through `ResourceServer`) and `pc-ps2` (its Irq and Ioport resources).
- A deliberate breakage, the source put back after (`cmp` against a saved copy): both `on_signals()` answering `Close` without calling `handle()`, and the rel image booted. `routing_checks` failed 8 checks (`Debug.Get`, `Irq.Get`, `Ioport.Get` and `Sampling.Get` = `PeerClosed` where `Ok` was expected; three Launches = `PeerClosed` where `AccessDenied` or `InvArgs` was; and a spawn = `NoResource`, since the entries never freed), the shell's `Debug.Get`, `Sampling.Get` and root job checks failed, and `pc-ps2` could not get its I/O port resource. So both new paths are on the boot path and the boot depends on them.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/system/service/init/.*'`, on `resource_server.cpp` and `launcher_server.cpp`: exit 0, no diagnostic. `scripts/format_cpp.py --check` on the four files: clean.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-initservers`, in `.worktrees/lw-initservers` with the final tree: dbg PASS (shell finished at 5.9 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `routing_checks: passed`, `pc-ps2: key a` and `pc-ps2: key A`; both logs hold `shell: loop checks passed`.
- Not measured: the time of one message through a connection before and after, and anything on x86-64 hardware.

**Deliberately not done**
- The resource servers' pools stay in `.data`; see `docs/repairs/261029/init.md`.
- `Realm`'s two registrations, `LoopServer`, `Outgoing`, the echo example and the shell's `check_loop()` still use the function form: steps 2, 4, 5 and 6 of the plan.
- No deleted copy on `ResourceConnection`: the old class was copyable and nothing copies it; `LauncherConnection` still deletes its copy because its state points at it.
- No `static_assert` on the size of a connection: it is a cost, not an invariant.

**How it was found**
None. The `.data` growth was found by measuring `sizeof` and the sections of `init` before the change and again after it, as the brief asked, and by reading `llvm-nm -S`, which shows the five storages change from type `b` to `d`.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A connection can be the `Waiter` of its slot because it has one slot and one behaviour. Which object in the root has two waits with two behaviours, and why can it not be the `Waiter` of both?
2. `LauncherConnection` has two bases with a vptr each. If `bind()` had stored `&connection` as a `void *` and `call_waiter()` cast it to `Waiter *`, which function would the Loop have called, and what is it about the `Waiter &` overload that makes the mistake impossible?
3. Why does a vptr of 8 bytes in each of four connections move a 4 KiB message buffer from `.bss` into the file, and which of `constinit`, the buffer or the pointer is the cause?

## Title: A component instance's two waits are the Loop's Waiters, and the two static trampolines go (2026-10-29)

**Files**
- `system/service/init/realm.hpp` — `Instance` gets two private nested classes, `OutgoingWaiter` and `TerminatedWaiter` (each `final: public loop::Waiter` with a private `on_signals()` override and a back pointer to the `Instance`), two members of them, and a private `attach(Realm &)`; `Realm::on_outgoing` and `Realm::on_terminated` are private non-static members that take `Instance &`
- `system/service/init/realm.cpp` — the two `on_signals()` bodies hand the call to the Realm; `resolve_root()` and `resolve()` call `attach()` where they set `m_realm`; the two `add()` calls pass the waiters; the two handlers keep their bodies and take `Instance &`; the static `on_outgoing(void *, Signals)` and `on_terminated(void *, Signals)` are gone
- `.agents/tasks/loop-waiter-realm/PLAN.md`, `RESULT.md` — this step's workspace

**Symptom**
Latent, cannot be triggered today: the root behaves as before. This is step 5 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, the last caller of the function-pointer form of `Loop::add()`. Each wait of an instance was registered with a static function and the instance as an untyped pointer:

```cpp
// before
m_loop->add(outgoing, Signals::Readable | Signals::PeerClosed, &Realm::on_outgoing, &instance);
m_loop->add(started->process, Signals::Terminated, &Realm::on_terminated, &instance);

auto Realm::on_outgoing(void *const context, Signals) -> loop::Action {
    auto *const instance = static_cast<Instance *>(context);
    instance->m_outgoing = Uandle::Invalid;
    return loop::Action::Close;
}
```

**Cause**
Why `Instance` cannot be the `Waiter` of its own waits, as a connection was in step 3: a `Waiter` has one `on_signals()`, and an instance has two waits with two behaviours. The outgoing directory's client end fires on Readable or PeerClosed and the instance stops naming the handle; the process fires on Terminated and the instance reports its exit code and becomes Exited. A single `on_signals()` would have to find out which wait fired, and the only thing it is given is `observed`, the signals the packet's object asserted. That does not say which object it was: a signal bit is read per object type (`<ours/signals.hpp>`: bit 3 is Signaled on an event and Terminated on a task), so a `switch` on `observed` works only for as long as no object type is waited on whose bits overlap, and nothing enforces that. The other way out is a tag set at registration, which is a second member of the instance in disguise, with the `switch` on top. Two members, one per wait, make the choice by the address the Loop was given, which cannot be wrong.

What it saves and what it costs. It saves the two static trampolines and their two `static_cast`s from `void *`: the instance is a typed `Instance &` from the waiter's back pointer on. It saves no machine code (see Verification). It costs two vptrs and two back pointers in every instance, 32 bytes, over a pool of 8.

The trap in the back pointer. `Realm` sets an entry up with `child = Instance{}`, which assigns every field, so it assigns the waiters' fields too. A constructor that pointed each waiter at `this` would have worked for the temporary and then been copied over the entry: the entry's waiters would name a temporary that is gone at the end of the statement, and the first wait to fire would call through a dangling pointer. So the back pointers are set after the reset, by `attach()`, in the two places that already set `m_realm` and are the only two places an entry is reset: `resolve_root()` (the root) and `resolve()` (the children it allocates). `attach()` sets `m_realm` too, since the waiters reach the Realm through it. The vptr is not copied by an assignment, which is what lets the reset leave a working vtable in place.

Why `attach()` is a member of `Instance` and not four lines in `Realm`: the nested classes' back pointers are private to them (only `Instance` is their friend), and the instance is the one that knows what it takes for its waiters to name it. `Realm` is a friend of `Instance`, so it reaches `attach()`, `m_outgoing_waiter` and `m_terminated_waiter`, and passes the members to `add()` as `Waiter &`: the base is public, so the conversion is the compiler's, which is what the `Waiter &` overload is for.

Access: a nested class of a befriended class. `Realm` has `friend class Instance;`, and the waiters call `Realm::on_outgoing()`, which is private. A friend's grant covers the member declarations of the befriended class, and a nested class's member functions are among them, so no `friend class Instance::OutgoingWaiter` was needed; it compiled the first time. The `on_signals()` overrides are private for the reason the connections' are: only the Loop calls them, and it calls them through `Waiter`, where they are public.

Why the handlers stay members of `Realm` and take `Instance &`, and why not static. They are the supervision of instances, which is the Realm's job, and `Instance` already depends on `Realm` (`m_realm`, `resolve()`), so the waiters add no new direction. They use no `Realm` state today, which is true of `forward()` as well; clang-tidy's `readability-convert-member-functions-to-static` would name all three, and the repository does not enable that check. Static members that take `Instance &` would have needed no `m_realm` in the waiters; the brief chose members, and the cost of the choice is one load, the waiter's `m_instance->m_realm`.

Lifetime, which `Waiter` requires to be stated: the object stays where it is from `add()` until the slot is freed. The waiters are members of pool entries; the pool is `INSTANCES`, a global `ustl::LazyInit` that never moves, and an instance is never destroyed (D27), so the address is stable for the process's life. Whether a registered waiter can be registered again while its slot is in use, checked path by path:
- The outgoing waiter is registered in `resolve()`, which returns at its first line for an instance that is not `Unresolved`. After a successful `add()` nothing in `resolve()` can fail (what is left is stores and the children loop, which has no `return`), and the instance ends `Resolved`. So an instance whose outgoing waiter is registered never resolves again. A failed `add()` registered nothing.
- The terminated waiter is registered at the end of `run()`, after every failure return, and `run()` is called only from `start()`, which returns at its first line for an instance that is not `Resolved`. `run()` sets `Started` just before the `add()`. A second `start()` finds `Started` or `Exited` and returns. A failed `add()` registered nothing.
- A pool entry is reset once. `resolve_root()` resets the root, once, at boot; `resolve()` resets the children it allocates at `m_used + index` after checking that room, and `m_used` only grows. (A second `resolve_root()` would reset an entry whose waiters are registered and null their back pointers, so the next wait to fire would dereference null. Under the old form the same call would already have orphaned the root's slots, since the instance would no longer name their handles. Nobody makes it, and `resolve_root()` is documented as the boot step that does it once.)
- Both handlers answer Close, so the Loop never re-arms either wait. The path that frees a slot without the waiter's Close, a failed re-arm (`docs/repairs/261029/ioservice.md` item 1), cannot happen to them.

The worst interleaving for a Waiter is a slot freed and its object reused while a packet from the old life is queued. Here the object is never reused (no entry is reset twice), and the Loop drops a stale packet at step 1 of `run()` by its generation in any case. The root runs the Loop and every registration on one thread (D16), and `on_signals()` does a load and a call, so nothing allocates or locks.

**Change**
```cpp
// realm.hpp, in Instance (private)
class OutgoingWaiter final: public loop::Waiter {
  public:
    constexpr OutgoingWaiter() = default;

  private:
    friend class Instance;
    auto on_signals(ours::Signals observed) -> loop::Action override;
    Instance *m_instance = nullptr;
};
// TerminatedWaiter alike

auto attach(Realm &realm) -> void {
    m_realm = &realm;
    m_outgoing_waiter.m_instance = this;
    m_terminated_waiter.m_instance = this;
}

OutgoingWaiter m_outgoing_waiter;
TerminatedWaiter m_terminated_waiter;

// realm.cpp
auto Instance::OutgoingWaiter::on_signals(Signals) -> loop::Action {
    return m_instance->m_realm->on_outgoing(*m_instance);
}
// resolve_root(), resolve()
root.attach(*this);   // was: root.m_realm = this;
child.attach(*this);  // was: child.m_realm = this;
// registrations
m_loop->add(outgoing, Signals::Readable | Signals::PeerClosed, instance.m_outgoing_waiter);
m_loop->add(started->process, Signals::Terminated, instance.m_terminated_waiter);
// handlers: Realm::on_outgoing(Instance &instance), Realm::on_terminated(Instance &instance),
// bodies as before with instance-> turned into instance.
```
The comments that named the callbacks, the context or the slot's callback now name the waiters or `on_signals()`: the descriptions of `Instance` and `Realm`, the two handlers, the registrations' comments and `m_outgoing` and `m_process`. The comments that say a slot closes a handle (I3) say the Loop closes it once the waiter answers Close. Rejected: one `Instance` that is the `Waiter` and switches on `observed`, for the reason above; setting the back pointers in a constructor, for the reset above; `offsetof` from the waiter to its instance, which is undefined on a class with a virtual function; `init.cpp` or `pools.hpp` changes, which nothing forced.

**Reference**
None, this project's own design. It follows `docs/changes/261029/loop.md` and the sections of steps 3 and 4, and the abstract classes of 2026-10-28.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` (the whole default build) succeeds. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 13, `host/obj/system/service/init/test/autorun` 5; `boot_check.py` runs all 148 host tests of `//:default` and passes them.
- `grep -rn "Callback\b" system examples --include='*.cpp' --include='*.hpp'` finds only the Loop's own: `loop/include/loop/loop.hpp` (the comment on the form, the `using Callback`, `Slot::m_callback`, the old `add()`), `loop/src/loop.cpp` (the old `add()`) and one test name in `loop/test/loop_test.cpp`. No caller outside the Loop uses the old form any more; step 6 removes it.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/system/service/init/.*'`, on `realm.cpp`: exit 0, no diagnostic (checked to analyse the file: with `-checks='-*,readability-convert-member-functions-to-static'` it names `forward`, `on_outgoing` and `on_terminated`). `scripts/format_cpp.py --check` on both files: clean.
- Sizes, from `-fdump-record-layouts-simple` on the compile command of `realm.cpp` and from `llvm-size -A`, `llvm-nm -S` and `llvm-readelf` on `init` before and after:

| | Before | After |
|---|---|---|
| `sizeof(Instance)` | 224 | 256 |
| `OutgoingWaiter`, `TerminatedWaiter` | none | 16 each (vptr, back pointer) |
| `INSTANCES` (8 instances) | 1792, type `b` | 2048, type `b` |
| `.data` | 51360 | 51360 |
| `.bss` | 56448 | 56704 |
| `.text` | 108846 | 109278 |
| `.data.rel.ro` | 2248 | 2296 |
| `.rela.dyn` | 5664 | 5712 |
| `init` stripped (`llvm-objcopy --strip-all`) | 177,648 | 178,176 |

  Nothing moved from `.bss` to `.data`: `INSTANCES` is a `LazyInit`, whose storage is zeros until `main()` builds the array at run time, so the vptrs are stored by `std::array<Instance, 8>::array()` and not written into the image. This is the difference from the resource servers' pools (`docs/repairs/261029/init.md`), which are `constinit` objects with their vptrs. `.data` is the same 51360 bytes and the second RW `LOAD` segment's file size is 0xc8a0 in both; the `.bss` growth is the 256 bytes of the pool. The 48 bytes more in `.data.rel.ro` and `.rela.dyn` are the two vtables (24 bytes each) and their relocations.
  `.text` grew by 432 bytes (413 in symbols, the rest alignment), nearly all in the one place that stores the vptrs: `std::array<Instance, 8>::array()` went from 930 to 1240 bytes, since the eight constructors are unrolled and each now stores two. `resolve()` grew by 69 bytes and `resolve_root()` by 18 (the reset assigns 32 bytes more); the two `on_signals()` are 22 and 17 bytes; `on_outgoing` is 18 bytes before and after, and `on_terminated` 366 before and 363 after.
- Deliberate breakages, each followed by `cmp` against the saved copy of the source and a build and boot of the rel image (`boot_check.py --out out/lw-realm --only rel --no-tests`):
  - `TerminatedWaiter::on_signals()` answering `Close` without calling the Realm: `rel FAIL  no "init: echo exited with 0" within 60s`, 41 passed, 8 skipped, 0 failed, 5/6 required lines. No `init: <name> exited with` line is logged at all, since `on_terminated` is the only place that logs it; `routing_checks: passed` is still logged, because the route to the exited echo answers NotFound from the outgoing wait alone.
  - `OutgoingWaiter::on_signals()` answering `Close` without clearing `m_outgoing`: **the rel image still passed**, 6/6 required lines, `routing_checks: passed`. The boot does not depend on the clearing. `Realm::forward()` answers NotFound when `m_state == Exited` or `m_outgoing == Invalid`, and by the time `routing_checks` connects again after `Stop` the TERMINATED packet has been handled, so the state alone answers (no `init: cannot forward` line was logged, which a connect that reached a closed `m_outgoing` would have caused); and `check_echo()` allows one PeerClosed and retries. What the clearing guards is the window in which a provider's outgoing directory has closed and its process has not yet been reported, or a provider that closes its outgoing directory and keeps running; neither is in the boot.
  - To show the outgoing path is on the boot path at all, `OutgoingWaiter::on_signals()` was replaced by `__builtin_trap()`: `rel FAIL  6 FAILED line(s)`, 4/6 required, with `routing_checks: /svc/ours.test.Echo = PeerClosed (expected Ok)`, `routing_checks: a route to echo once it has exited = BadState (expected NotFound)`, `routing_checks: 2 check(s) FAILED` and the shell's `launcher service checks`, `profile checks` and `spawn checks` FAILED, since init itself died at the first wait that fired on an outgoing directory.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lw-realm`, in `.worktrees/lw-realm` with the final tree: dbg PASS (shell finished at 5.5 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.6 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, `routing_checks: passed`, `init: echo exited with 0`, `pc-ps2: key a` and `pc-ps2: key A`); host tests PASS (148 passed, 0 failed). Both logs hold `shell: loop checks passed`.
- Not measured: the time of one wait's dispatch before and after, and anything on x86-64 hardware.

**Deliberately not done**
- The old `Loop::add(handle, signals, Callback, void *)` and `Callback` stay; step 6 removes them with the shell's `check_loop()` and the loop tests moved.
- No host test: `Realm` calls `channel_create`, `launch::start()` and `object_get_info`, and the host has a fake of the kernel only for `ioservice` and `loop`. The boot is the test, and the breakages above show which part of it each waiter carries and which it does not.
- No `static_assert` on `sizeof(Instance)`: it is a cost, not an invariant.
- `OutgoingWaiter` and `TerminatedWaiter` are two classes, not one template over the handler: the bodies are one line each, and a template over a member-function pointer adds a name and no saving.
- `init.cpp`, `pools.hpp` and the BUILD file did not change: the pool is still `LazyInit<std::array<Instance, MAX_INSTANCES>>`, built by `INSTANCES.init()`.

**How it was found**
None. The waiter's back pointer hazard was found by reading how `Realm` sets an entry up (`child = Instance{}`) before choosing where to set the pointer; the size and section figures by measuring before and after, as the brief asked; and the gap in the boot's coverage by the second breakage, which passed.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A connection is the `Waiter` of its one wait, but an `Instance` is not the `Waiter` of its two. What does `on_signals()` receive, and why is that not enough to tell the two waits apart, even when today's signal bits happen not to overlap?
2. `Realm` resets an entry with `child = Instance{}`. If `Instance`'s constructor had pointed each waiter's back pointer at `this`, what would the entry's waiters name after the assignment, and when would it show?
3. Making `OutgoingWaiter` answer Close without clearing `m_outgoing` did not fail the boot. Which test in `Realm::forward()` hides it, and what would a provider have to do for the stale handle number to be used?

## Title: The resource servers' storage is built at run time, as the other servers' is (2026-10-30)

**Files**
- `system/service/init/init.cpp` — `DEBUG_RESOURCE_STORAGE`, `IRQ_RESOURCE_STORAGE`, `IOPORT_RESOURCE_STORAGE`, `SAMPLING_RESOURCE_STORAGE` and `ROOT_JOB_STORAGE` are `constinit ustl::LazyInit<init::ResourceServer::Storage>` and `main()` builds each with `*X.init()` as it makes the server; the comment above the globals says why
- `docs/repairs/261029/init.md` — item 1 (the pools in `.data`) deleted, since this fixes it
- `.agents/tasks/init-lazy-storage/RESULT.md` — this task's workspace

**Symptom**
The `.data` growth recorded as item 1 of `docs/repairs/261029/init.md` and measured in the section of this file about the Resource and Launcher connections: since `ResourceConnection` became a `loop::Waiter`, each `ResourceServer::Storage` was type `d` in `llvm-nm`, not `b`, and `init` carried 22 KiB of zeros in its file. The program behaved as before.

**Cause**
A file stores `.data` and does not store `.bss`. The linker puts a global in `.bss` only if every byte of its initial value is zero; one non-zero byte puts the whole object in `.data`, and the file then holds all of it. A `Storage` is one object: four `ResourceConnection`s of 24 bytes each, then the `request_bytes` buffer of 4 KiB and the small arrays beside it. Each connection is a `Waiter`, so it starts with a vptr, which is the address of its vtable and so not zero. `constinit` made it worse than it needed to be: it forces the compiler to evaluate the initial value at compile time and write the result into the image, so the vptr was stored in the file together with 4 KiB of message buffers that hold nothing until a message arrives.

`constinit` was never needed here, and it was only ever free while the object happened to be all zeros. The reason a `Storage` is a global at all is its size (about 4.4 KiB each, five of them, far more than the stack), and the reason it is initialised before any program code is that nothing in a user program runs dynamic initialisers. `ustl::LazyInit<T>` meets both: its own constructor is `constexpr` and does nothing, so the global is constant-initialised to all zeros and lies in `.bss`, and `main()` constructs the `Storage` in it when it makes the server, writing the vptrs at run time. Nothing about a resource server needs the `Storage` to exist before `main()`: the server is made in `main()` from it, and no other code reaches the global. The other servers already did this, for the other reason in the comment (a destructor `__cxa_atexit` would register): `BOOT_STORAGE`, `INSTANCES` (`LazyInit<std::array<Instance, 8>>`, whose measurement is in the section on a component instance's two waits) and the echo server's storage.

**Change**
```cpp
// before
constinit init::ResourceServer::Storage DEBUG_RESOURCE_STORAGE{};
init::ResourceServer debug_resource_server(DEBUG_RESOURCE_STORAGE, loop,
                                           ours_resource::DEBUG_PROTOCOL, debug);
// after
constinit ustl::LazyInit<init::ResourceServer::Storage> DEBUG_RESOURCE_STORAGE;
init::ResourceServer debug_resource_server(*DEBUG_RESOURCE_STORAGE.init(), loop,
                                           ours_resource::DEBUG_PROTOCOL, debug);
```
The same for the other four. `init()` is called exactly once per global, in the one place that makes its server, before the server can bind a connection: `LazyInit::init()` is `std::construct_at(data())`, which value-initialises the `Storage`, so the 4.4 KiB is zeroed once more (already zero in `.bss`, but the compiler cannot know that) and the four vptrs are stored. `LazyInit` has no destructor to run, and the `Storage` has none that matters, since the root never exits (the same reason as for the others).

What stays the same: RAM. `.data` plus `.bss` is 106,656 bytes before and after, the RW segment's memory size is 0x1a0a8 in both, and the five storages are the same 4480 bytes each at run time. What changes is how many bytes of the file the loader has to carry for init: 22 KiB fewer.

Rejected: moving only the message buffers out of `Storage` into their own `.bss` globals, or sharing one set of buffers among the five servers (the first two options of the repair item). Both change `resource_server.hpp`, where another agent is working, and the second changes who owns the buffers, which is the user's decision. This is the one-line version of the first option that touches only `init.cpp`.

**Reference**
None, this project's own design: `BOOT_STORAGE` and `INSTANCES` in the same file, and the section of this file on a component instance's two waits, where `INSTANCES` is shown to stay in `.bss` for this reason.

**Verification**
- `prebuilt/ninja/ninja -C out/init-lazy` (the whole default build) succeeds, and `boot_check.py --out out/init-lazy` passes: dbg PASS (shell finished at 5.8 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.9 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, which includes `routing_checks: passed`); host tests PASS (148 passed, 0 failed).
- clang-tidy on `init.cpp`, against `out/init-lazy`, `--header-filter='.*/system/service/init/.*'`: exit 0, no diagnostic (checked to analyse the file: with `-checks='-*,readability-convert-member-functions-to-static,misc-const-correctness'` it reports 13). `scripts/format_cpp.py --check` on it: clean.
- Sizes of `init`, built from the tree as it was (`a8489d18`) and from the change, `llvm-size -A`, `llvm-nm -S -C`, `llvm-readelf -l` and `llvm-objcopy --strip-all`:

| | Before | After |
|---|---|---|
| `.text` | 108,878 | 109,166 |
| `.data` | 51,360 | 28,960 |
| `.bss` | 55,296 | 77,696 |
| `.data.rel.ro` | 2,296 | 2,296 |
| `.rela.dyn` | 5,712 | 5,232 |
| `.data` + `.bss` | 106,656 | 106,656 |
| Second RW `LOAD`, file size / memory size | 0xc8a0 / 0x1a0a8 | 0x7120 / 0x1a0a8 |
| `init` stripped (`llvm-objcopy --strip-all`) | 177,776 | 155,184 |
| The five `*_STORAGE` symbols, each | `d`, 0x1180 (4480) | `b`, 0x1180 (4480) |

  The stripped file is 22,592 bytes smaller (22.06 KiB). `.data` is now `LAUNCHER_STORAGE` alone (type `d`, 0x7120 = 28,960 bytes), which is `docs/repairs/261004/init.md` item 9. `.text` is 288 bytes larger, all in `main()` (0x8b6 to 0x9d6 bytes, the only text symbol that changed): the five constructions, each a zeroing of the `Storage` and four vptr stores. `.rela.dyn` is 480 bytes smaller: the 20 vptrs (5 storages of 4 connections) no longer need a load-time relocation of 24 bytes each, since `main()` stores the vtable's address itself.
- A breakage, to show the construction is on the boot path: `DEBUG_RESOURCE_STORAGE.init()` removed (the call became `*DEBUG_RESOURCE_STORAGE`, a reference to storage that was never built), a build, `boot_check.py --out out/init-lazy --no-tests`, then `cmp` of `init.cpp` against the saved copy. The rel image FAILED: 30 FAILED lines, 1 of 6 required lines, `shell: Debug.Get FAILED: PeerClosed`, and every `routing_checks` line that needs init to answer `PeerClosed`, because `init` was terminated by `FatalPageFault` with `cr2=0x0`, which is what a virtual call through the zero vptr of a connection that was never built does. The dbg image still PASSED: its log holds no `Debug.Get` line, since only the rel image's shell and `routing_checks` use the resource protocols. The restored source was built and booted again (the numbers above).
- Not measured: the time init takes to start, before and after (the zeroing is 22 KiB once), and anything on x86-64 hardware.

**Deliberately not done**
- `resource_server.hpp` still says a `ResourceConnection` array is "a constinit global of the root's" (the comments on `ResourceConnection` and on `Storage`, and "the pool is a global that never moves"): those files are being changed by another agent, and a global that never moves is still true of a `LazyInit` global. The comments are stale in the word `constinit` only.
- The Launcher's pool (`LAUNCHER_STORAGE`, 28,960 bytes, which is all that is left in `.data`) is not part of this change: it is `docs/repairs/261004/init.md` item 9, and the next commit's.
- No change to `Storage`'s layout or to the pool sizes: the memory the root keeps is the same.

**How it was found**
None. It is the first option of the repair item and the maintainer's decision: the sizes were taken before and after, as the repair item's Acceptance asks.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A `Storage` is one object, with 4 KiB of message buffers that are zero and four vptrs that are not. Why did the buffers end up in the file, and why would changing only the vptrs' storage class not have helped?
2. `constinit ustl::LazyInit<T> X;` is still `constinit`. Why does that put `X` in `.bss`, and what does `X.init()` do, and when, that `constinit` on a `T` did at compile time?
3. `.data` plus `.bss` is the same before and after, and so is the memory the root keeps. What did get smaller, and what does the loader do with `.bss` that it does not do with `.data`?

## Title: The Launcher's storage is built at run time too (2026-10-30)

**Files**
- `system/service/init/init.cpp` — `LAUNCHER_STORAGE` is `constinit ustl::LazyInit<init::LauncherServer::Storage>`, built by `main()` with `*LAUNCHER_STORAGE.init()` as it makes the Launcher; its `[[clang::no_destroy]]` and its own comment are gone, and the comment above the globals covers it
- `docs/repairs/261004/init.md` — item 9 rewritten for what remains (the pool of four); the part about init's image is fixed
- `.agents/tasks/init-lazy-storage/RESULT.md` — the second commit added to it

**Symptom**
The same as the previous section, for the Launcher: `LAUNCHER_STORAGE` was type `d` in `llvm-nm`, 28,960 bytes (0x7120), and it was all that was left in init's `.data`. Four connections of about 5.1 KiB each, whose `ProcessLauncherState` is nearly all zeros, and 8 KiB of message buffers, were stored in the file. Item 9 of `docs/repairs/261004/init.md` recorded it.

**Cause**
The previous section's, with two differences. The vptrs are two per connection, `LauncherOps` and `Waiter`, since a `LauncherConnection` derives from both; and its `ProcessLauncherState` is wired to its `LauncherOps` by the connection's constructor, `m_state(*this)`, so each connection holds a pointer to itself as well. That made the pool the reason for `constinit` in the first place (the comment in `init.cpp` said the hooks were "written into the image" so that "nothing need be built in `main()`"), and it is why it looked as if it could not move. It can: the constructor is `constexpr`, so it runs just as well at run time, and `std::construct_at` runs it on the address the `LazyInit` storage already has. The entry never moves (the copy and assignment are deleted, and the pool is a global), so a pointer to itself written at run time is as good as one written by the linker. `readelf -r` on `init` before shows 12 relocations inside `LAUNCHER_STORAGE`, three per connection (the two vptrs and the self pointer), each at the same offset in the four entries.

`[[clang::no_destroy]]` is not needed any more. It kept the compiler from registering the state's non-trivial destructor with `__cxa_atexit`; a `LazyInit` is raw bytes with a trivial destructor of its own, and nothing destroys the object inside it, which is what the root wants since it never exits and a connection that ends is reset, not destroyed.

**Change**
```cpp
// before
[[clang::no_destroy]]
constinit init::LauncherServer::Storage LAUNCHER_STORAGE{};
init::LauncherServer launcher_server(LAUNCHER_STORAGE, loop, environment, ...);
// after
constinit ustl::LazyInit<init::LauncherServer::Storage> LAUNCHER_STORAGE;
init::LauncherServer launcher_server(*LAUNCHER_STORAGE.init(), loop, environment, ...);
```
The comment above the globals is one list now: the destructors (the first reason) include the Launcher's, and the vptr and the file (the second) its pool, 28 KiB; the separate comment about the Launcher's compile-time wiring is gone. The same as before, RAM: `.data` plus `.bss` is 106,656 bytes in all three builds.

`launcher_server.hpp` is not changed: its comments (on `LauncherConnection`, "The pool is a constinit global of the root's, and [[clang::no_destroy]]", and on `Storage`) are stale in those words and in the reason they give for the constructor being `constexpr`. The file is being changed by another agent. The constructor can stay `constexpr`: a `constexpr` constructor that runs at run time is not wrong, only no longer needed at compile time.

**Reference**
None, this project's own design. The previous section, `BOOT_STORAGE` and `INSTANCES`.

**Verification**
- The tree builds and `boot_check.py --out out/init-lazy` passes: dbg PASS (5.5 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.5 s, 41 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines, `routing_checks: passed`, which connects to the Launcher and spawns a child through it); host tests PASS (148 passed, 0 failed). clang-tidy on `init.cpp` against `out/init-lazy`, `--header-filter='.*/system/service/init/.*'`: exit 0, no diagnostic (13 with the two ad-hoc checks, as before). `scripts/format_cpp.py --check`: clean.
- Sizes of `init`, the tree as it was (`a8489d18`), after the previous section's change, and after this one:

| | `a8489d18` | After the resource storages | After this |
|---|---|---|---|
| `.text` | 108,878 | 109,166 | 109,502 |
| `.data` | 51,360 | 28,960 | none (the section is gone) |
| `.bss` | 55,296 | 77,696 | 106,656 |
| `.data.rel.ro` | 2,296 | 2,296 | 2,296 |
| `.rela.dyn` | 5,712 | 5,232 | 4,944 |
| `.data` + `.bss` | 106,656 | 106,656 | 106,656 |
| Second RW `LOAD`, file size / memory size | 0xc8a0 / 0x1a0a8 | 0x7120 / 0x1a0a8 | 0 / 0x1a0a0 |
| `init` stripped (`llvm-objcopy --strip-all`) | 177,776 | 155,184 | 126,208 |
| `LAUNCHER_STORAGE` (0x7120 = 28,960 bytes) | `d` | `d` | `b` |

  The stripped file is 28,976 bytes smaller than after the previous section and 51,568 smaller than before it, and the whole of init's 106,656 bytes of writable data is `.bss`: the RW segment that held `.data` has no file bytes left. `.text` is 336 bytes larger: `LazyInit<LauncherServer::Storage>::init()` is a function of its own, 320 bytes, and `main()` grew by 16; those are the only text symbols that changed. `.rela.dyn` is 288 bytes smaller: the 12 relocations above.
- A breakage: `LAUNCHER_STORAGE.init()` removed from the call, a build, `boot_check.py --out out/init-lazy --no-tests`, then `cmp` against the saved copy of `init.cpp`. The rel image FAILED: 11 FAILED lines, 4 of 6 required lines, `routing_checks: /svc/ours.process.Launcher = PeerClosed (expected Ok)` three times, `spawn of routing_checks child = PeerClosed (expected Ok)`, and the shell's `launcher service checks` and `profile checks`, because `init` was terminated by `FatalPageFault` at `cr2=0x0` on the first message on a Launcher connection. The dbg image still PASSED, as with the resource storages: its shell logs `launcher service checks skipped (no namespace)`, so it never opens a Launcher connection. The restored source was built and booted again, and the `init` ELF is byte-identical to the one measured above.
- Not measured: the time init takes to start, before and after, and anything on x86-64 hardware.

**Deliberately not done**
- Item 9 of `docs/repairs/261004/init.md` was not deleted whole, as it was asked: only its cost in init's image is fixed. The pool of four (D27, a fifth connection answers NoResource) is a capacity choice that this does not touch, so the item is rewritten for that, with its Acceptance, and the user can delete it if the capacity is settled.
- The pool of four is not resized, and the Launcher's message buffers are not moved or shared. They no longer cost the file anything, and sharing them is a question of who owns them.
- `launcher_server.hpp` and `resource_server.hpp` keep comments that say `constinit`; see above and the previous section.

**How it was found**
None. It is the repair item's own first option in its simplest form, applied after the previous section showed that a `LazyInit` global wired by a constructor at run time reproduces what `constinit` did.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The comment said the Launcher's hooks "point back at its own entry, which constinit can write into the image". Why does the same constructor give the same result at run time, and what property of the entry makes a pointer to itself safe to write there?
2. `[[clang::no_destroy]]` is gone and no `__cxa_atexit` call appeared. Which object no longer has a destructor to register, and what would happen to the connection's state at the end of the process?
3. `.data` is not just smaller but absent. What in the section listing and in `llvm-readelf -l` shows it, and what does that say about the file bytes of the second RW segment?

## Title: the root's resource and launcher connections are released when the Loop cannot arm their wait again (2026-10-30)

**Files**
- `system/service/init/resource_server.hpp`, `resource_server.cpp` — `ResourceConnection::on_rearm_failed(cause)` and `ResourceServer::rearm_failed(connection, cause)`, which frees the entry and says so on the debug console; the comment of `ResourceConnection` says why the Loop's keeping of the entry's address still holds
- `system/service/init/launcher_server.hpp`, `launcher_server.cpp` — the same for `LauncherConnection` and `LauncherServer`; `rearm_failed()` also resets the connection's state through `release()`
- `system/service/init/realm.hpp` — the comments of `Instance::OutgoingWaiter` and `Instance::TerminatedWaiter` say why they keep the default `on_rearm_failed()`

**Symptom**
Latent, cannot be triggered today, and never listed: `docs/repairs/261029/echo.md` said the root's pools "have the same property" and left them out. Were `object_wait_async` to refuse the re-arm of a connection's wait, the Loop closed the channel and freed the slot and the entry stayed in use: `m_channel` was still set, so `bind()` skipped it for ever. A pool has four entries (`MAX_RESOURCE_CONNECTIONS`, `MAX_LAUNCHER_CONNECTIONS` in `pools.hpp`), five resource servers and one launcher, so four such events on one server and it answered `NoResource` to every later Open of its protocol. The launcher's entry also kept its `ProcessLauncherState`, with whatever a client had added (handles, a job) and not yet launched, open for good.

**Cause**
The same as `ioservice.md` of this day, in two more places. `ResourceServer::handle()` and `LauncherServer::handle()` free an entry only on the paths where they answer `Close` (`release`), and the Loop never told them that it had freed a slot for another reason. Both answer `Rearm` after a request that went well, and that is the answer whose re-arm can fail.

Why the launcher's release is not just `m_channel = Invalid`. `LauncherServer::release()` first calls `m_state.reset()`, which closes everything the client added and the state still holds (`AddHandles`, the job, the executable) and makes the state a new connection's. A failed re-arm is a connection ending like any other, so the same `release()` runs: skipping the reset would hand the next client of the entry another client's handles. That is why `rearm_failed()` of the launcher calls the existing static `release()` and does not write `m_channel` itself; the resource server has nothing but `m_channel` to free, and writes it.

Why the comments about reuse stay true. Both classes said an entry is bound again only after the Loop has freed the slot it released, because `handle()` answers `Close` and the Loop then frees the slot before any other callback runs. A failed re-arm runs the other way round, free then release, which is the same guarantee: the entry is free to bind only after both. The one thread that runs the Loop is also the one that binds, so nothing sits between the Loop's free and the callback. The comments now say both paths.

Why `OutgoingWaiter` and `TerminatedWaiter` need nothing. Both answer `Close` and nothing else (`Realm::on_outgoing()` and `on_terminated()`), so the Loop never arms their waits again and never calls `on_rearm_failed()` on them. A comment says so in each class, so that the next reader does not take the missing override for an oversight; they keep the default.

**Change**
```cpp
// resource_server.hpp, ResourceConnection (private)
auto on_signals(ours::Signals observed) -> loop::Action override;
auto on_rearm_failed(ours::Status cause) -> void override;

// resource_server.cpp
auto ResourceConnection::on_rearm_failed(Status const cause) -> void {
    m_server->rearm_failed(*this, cause);
}

auto ResourceServer::rearm_failed(ResourceConnection &connection, Status const cause) -> void {
    connection.m_channel = Uandle::Invalid;
    auto _ = ort::debug_line(
        {"init: ", m_protocol.name, " cannot wait on a client again: ", ours::to_string(cause)});
}

// launcher_server.cpp
auto LauncherServer::rearm_failed(LauncherConnection &connection, Status const cause) -> void {
    release(connection);
    auto _ = ort::debug_line(
        {"init: the Launcher cannot wait on a client again: ", ours::to_string(cause)});
}
```
The cause is logged with the debug line each file already uses for a reply that cannot be written; init links `ort`, so no dependency is new. The entry is freed before the line is written, so a failing console cannot hold it.

**Reference**
None, this project's own design. It uses the notification of `docs/changes/261029/loop.md` of this day.

**Verification**
- `prebuilt/ninja/ninja -C out/bringup.x86` builds init with `-Werror`; clang-tidy against `out/bringup.x86`, `--header-filter` on `system/service/init`, on `resource_server.cpp`, `launcher_server.cpp` and `realm.cpp`: no diagnostic. `format_cpp.py --check`: clean. `lint_docs.py --changed`: no violations.
- No host test: both servers call `channel_read`, `channel_write`, `handle_duplicate` and `launch::start()` themselves, and the host has a fake kernel for `ioservice` and `loop` only. What covers them is the compile (the overrides are `override`, so a mismatch of signature would not build) and the boot: the root serves the resource protocols and the launcher to `routing_checks` and the shell's launcher service checks, so the new vtable slots are in programs that run. The failing path cannot be provoked on the target: it needs the kernel to refuse `object_wait_async`, and no program here breaks the rules that would earn `BadHandle`. The Loop's side of it is pinned by `loop_test.cpp` and `loop_server_test.cpp`, whose `Binding` is the same pattern as these two.
- `boot_check.py --out out/lw-notify` with the final tree: dbg PASS, rel PASS (6/6 required lines, `routing_checks: passed`), host tests PASS; the numbers are in `loop.md` of this day. The sizes of `init` before and after are there as well: `ResourceServer::rearm_failed` is 154 bytes and `LauncherServer::rearm_failed` 120, mostly the debug line.
- Not measured: the failing path on the target.

**Deliberately not done**
- No test that fakes the root's servers. The pattern is the `Binding`'s, and a host fake of the channel and process system calls is a library, not a part of this change.
- `release()` of the launcher is not changed to take a cause, and the resource server has no `release()` to share: `handle()`'s lambda and `rearm_failed()` write the same one field, and a function for one assignment adds a name and no invariant.
- The 35 relocations of `ours::to_string` that `init`'s `.rela.dyn` gained in this build are not fixed here; see `docs/repairs/261029/abi.md`.

**How it was found**
From the note in `docs/repairs/261029/echo.md` (item 2, deleted by this change) that the root's pools "have the same property", and by reading both `handle()` functions: the same `release` that is only reached on a `Close`, the same `Rearm` answer after a good request, and an `m_channel` that marks the entry in use.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `LauncherServer::rearm_failed()` calls `release()` and `ResourceServer::rearm_failed()` writes `m_channel` itself. What does the launcher's entry hold that the resource server's does not, and what would the next client of the entry find if the launcher skipped the reset?
2. The Loop frees the slot and then calls `on_rearm_failed()`, while `handle()` releases the entry and then answers `Close`. Why does an entry become bindable only after both in either order?
3. `OutgoingWaiter` and `TerminatedWaiter` have no `on_rearm_failed()`. Which of their answers makes that safe, and what would make it unsafe?

## Title: a route to a provider that has exited is logged as a route that ends nowhere (2026-10-30)

**Files**
- `system/service/init/realm.cpp` — `Realm::forward()` takes the consumer and its use, and logs one line before the NotFound epitaph when the provider has exited or its outgoing directory is closed; `connect()` passes them
- `system/service/init/realm.hpp` — the declaration and comment of `forward()`
- `docs/repairs/261004/init.md` — item 10 deleted: it is this change

**Symptom**
On dev, routing_checks asks echo to stop and connects to `/svc/ours.test.Echo` again. The root answers the connection with a NotFound epitaph, as D23 says, and the console says nothing about it. Every other route that ends nowhere prints `init: <moniker>'s use of "<name>" routes nowhere: ...` (`log_route_error()`), so a reader of the log could see why a client lost a protocol in every case but this one. The log of a dev boot, before:

```text
echo: stopping, as a client asked
init: echo exited with 0
routing_checks: passed
```

**Cause**
The design treats a source that has exited as a routing error like any other: "The root sends an epitaph on `server` and logs one line naming the target, the hop and the capability ... A source that has exited also answers NotFound". In the code the two are decided in different places. `component::route()` walks manifests and knows nothing of instance states, so the walk to an exited echo succeeds and returns echo as the provider. It is `Realm::forward()`, which then finds `provider.m_state == InstanceState::Exited`, or `m_outgoing` already closed by the Loop, and answered `close_with_epitaph(channel, Status::NotFound)` without a line. `connect()` had logged the failed walks it found itself, with `log_route_error()`; `forward()` was one step further down and had neither the consumer nor the use to name, since `connect()` passed it only the provider, the name the provider knows the protocol by, and the channel.

The two states that send a route to this branch are not the same. `Exited` is set by `on_terminated()` (the process ended) and by `start()` (it never started). A closed `m_outgoing` is set by `on_outgoing()`, when the Loop saw PEER_CLOSED on the client end of the outgoing directory, which happens when the provider's handles close as it dies, and it can come before the TERMINATED packet. So a route can find the outgoing directory closed while the state still says `Started`, and the line says which.

**Change**
`forward()` is passed the consumer and the use, and writes `log_route_error()`'s line in its shape, with the provider as the place the walk stopped:

```cpp
// before
auto Realm::forward(Instance &provider, std::string_view const name, Uandle const channel) -> void {
    if (provider.m_state == InstanceState::Exited || provider.m_outgoing == Uandle::Invalid) {
        close_with_epitaph(channel, Status::NotFound);
        return;
    }

// after
auto Realm::forward(Instance const &consumer, component::Use const &use, Instance &provider,
                    std::string_view const name, Uandle const channel) -> void {
    if (provider.m_state == InstanceState::Exited || provider.m_outgoing == Uandle::Invalid) {
        auto why = std::string_view{"exited"};
        if (provider.m_state != InstanceState::Exited) {
            why = "its outgoing directory is closed";
        }
        auto _ = ort::debug_line({"init: ", Moniker(consumer).view(), "'s use of \"",
                                  use.source_name, "\" routes nowhere: NotFound at ",
                                  Moniker(provider).view(), " (", why, ")"});
        close_with_epitaph(channel, Status::NotFound);
        return;
    }
```

The line names the use as the consumer wrote it (`use.source_name`, `ours.test.Echo`), not the name the provider serves it under (`ours.examples.echo.Echo`), as every other `routes nowhere` line does: the consumer's manifest is where a reader starts. It allocates nothing: the pieces are views, and the two monikers are stack buffers of the instance depth. The epitaph is written after the line, as in `connect()`.

Ruled out: teaching `route()` about instance states, which would make the pure router depend on the root's lifecycle and break its host tests' model.

**Reference**
Fuchsia `RoutingError::RouteSourceShutdown` (`src/sys/lib/routing/src/error.rs:353`, mapped to NOT_FOUND at `:424`), which component_manager reports through the same path as every other routing error, so it is logged with the target's moniker.

**Verification**
- `boot_check.py --out out/log-silent` with this tree: dbg PASS (44 passed, 4 skipped, 0 failed), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host tests PASS (151): the counts of the tree before. `REQUIRED` is unchanged.
- The acceptance of the repair item, three dev boots (one inside `boot_check.py`, two more with `--only rel`). Each log has the same one line, after `init: echo exited with 0`:

```text
echo: stopping, as a client asked
init: echo exited with 0
init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)
routing_checks: passed
```

- No host test: the root needs the kernel (processes, a Loop on a real port), and `forward()` is reached through `connect()` with real instances. The boot is the test, and the same boot without this change is the "before" block above.
- clang-tidy against `out/log-silent`, `--header-filter` on `system/service/init`, on `realm.cpp`: no diagnostic. `format_cpp.py --check` and `lint_docs.py --changed`: clean.
- Not seen on the target: the second wording, for a provider whose outgoing directory is closed while its state is not `Exited`. It appears when the root handles routing_checks's Open between the PEER_CLOSED on echo's outgoing directory and the TERMINATED of its process; the kernel queues the first before the second (the comment in `routing_checks.cpp`'s `check_echo()`), and in all three boots the root had handled both by then, so every line says `exited`.

**Deliberately not done**
- No throttle and no once-per-provider memory. Each connect to a provider that is gone prints a line, as each failed walk in `connect()` already does unthrottled (`log_route_error()`): a component that retries a dead provider in a loop fills the root's console at the pace it can send Opens, and the line is what a reader needs to see that it does.
- The other two answers of `forward()` that are NotFound or an encode error (a name longer than ours.io allows, which the manifest compiler already refuses; a message that does not encode) still print nothing. They are not the case asked for and cannot be reached with a manifest the compiler accepted.
- The Open that is lost into a provider that is dying but not yet marked gone (the interleaving `check_echo()` describes) prints no line: it was forwarded, fire and forget, and the client sees PEER_CLOSED with no epitaph. The next connect, which finds the provider gone, prints.

**How it was found**
None for a bug in the code; the gap is item 10 of `docs/repairs/261004/init.md` (deleted here), found by reading a dev boot log, where the epitaph routing_checks checks for has no line beside it.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why can `component::route()` not report a provider that has exited, and where in the root is the first place that both knows the state and still has the consumer's use?
2. The line names `ours.test.Echo`, and the provider knows the protocol as `ours.examples.echo.Echo`. Which one does the log need, and who would be misled by the other?
3. `forward()` can find a provider gone with `m_state` still `Started`. Which two events set the two fields, which can come first, and what does the second wording of the line tell a reader that the first does not?

## Title: the root logs an Open of a /svc name the component does not use, and says nothing of /boot's refusals (2026-10-30)

**Files**
- `system/service/init/realm.cpp` — `Realm::open_refused()`, the root's override of the new `Delegate` hook: one line when the directory is a component's /svc; `key_instance()` and `key_use()`, the inverse of `service_key()`, which `connect()` uses too
- `system/service/init/realm.hpp` — the declaration and comment of `open_refused()`
- `system/service/init/boot_server.hpp`, `system/service/init/boot_server.cpp` — `BootServer::first_service(directory)`, how the root tells a /svc from /boot
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_undeclared()`, beside `check_unoffered()`: connects to a name its manifest does not use and expects NotFound; it is what puts the line in the dev boot log

**Symptom**
A component that has a `/svc` opens `/svc/<name>` for a name that is not one of the entries its manifest's protocol uses made. The root's /svc tree for that component refuses the Open with NotFound, the client reads the epitaph, and the root's console says nothing. Of the three ways a connect to a protocol ends in NotFound without a root line (chapter 5 of the developer's guide, causes 2, 4 and 5), this was the first: the one a manifest typo on the *consumer's* side makes. Before the root's line, with the new check in `routing_checks` and nothing else (the commit before this one, booted in a scratch worktree), the dev boot answers the check's connect with NotFound, so `routing_checks: passed`, and the log has nothing for it between the line for the unoffered protocol and echo's stop:

```text
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
echo: stopping, as a client asked
init: echo exited with 0
init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)
routing_checks: passed
```

A component with no protocol `use` at all has no `/svc` and is not this case: `fsio::open("/svc/x")` finds nothing in its namespace to resolve the path against and answers NotFound itself (`Namespace::resolve()`), without sending a message. The root never hears of it, so there is nothing for the root to log, and the guide's cause 1 stays as it is.

**Cause**
The refusal is made by the root's `BootServer`, an `ioservice::LoopServer` that serves /boot and every component's /svc from one tree and one connection table. Its `Refuse` case wrote the epitaph and had no word for the program that owned the server. The L3 change (`ioservice.md`, same day) gives `Delegate` a hook for it, `open_refused(directory, path, status)`; this change is the root using it.

The root cannot report every call of that hook. A refused Open on /boot is ordinary: a program that is not there (`routing_checks` asks `fsio::spawn()` for one on purpose), a file opened with rights the connection does not hold. Logging those would fill the console with lines nobody needs and bury the one that matters. So the root has to tell a /svc from /boot from what the hook gives it, and the hook gives it the directory the Open was made on.

**Change**
The tree tells them apart, with no new storage. Every child of a component's /svc is a service whose value is `service_key(pool index, use index)` (`serve_svc()`), and /boot and the subdirectories components hold as namespace entries contain no service at all. A directory whose first child is a service is therefore a /svc, and the key names the instance it belongs to:

```cpp
// boot_server.cpp
auto BootServer::first_service(ioservice::NodeId const directory) const -> ustl::Option<u64> {
    auto const child = m_tree.first_child(directory);
    if (child.is_none()) {
        return ustl::NONE;
    }
    return m_tree.service(*child);
}

// realm.cpp
auto Realm::open_refused(ioservice::NodeId const directory, std::string_view const path,
                         Status const status) -> void {
    auto const key = m_boot->first_service(directory);
    if (key.is_none()) {
        return;
    }
    auto const pool_index = key_instance(*key);
    if (pool_index >= m_used) {
        return;
    }
    std::array<char, MAX_LOGGED_PATH> shown{};
    auto _ = ort::debug_line(
        {"init: ", Moniker(m_pool[pool_index]).view(), "'s /svc refuses an open of \"",
         ioservice::printable_path(path, shown), "\": ", ours::to_string(status)});
}
```

The options for telling them apart:
- The tree, chosen: the answer is already in the nodes, `connect()` already takes the key apart the same way, and an instance whose /svc could not be made (no entry added) simply has none and is not reported.
- A field of `Instance` holding the node of its /svc, searched by the pool on each report: explicit, but a second place that must stay equal to what `serve_svc()` added, and a scan of the pool.
- A flag per connection: the hook does not receive the connection, and the `Server` has no notion of what a directory is for.

The line is `init: <moniker>'s /svc refuses an open of "<path>": <Status>`, in the shape of `init: <moniker>'s /svc cannot be served: <Status>` beside it. It says "refuses an open" and not "routes nowhere" because nothing was routed: the refusal happens before any route is walked, and it covers every refusal of the /svc tree, not only a missing name (a hand-built Open with a flag a service takes none of is `InvArgs`, a right the /svc connection does not hold is `AccessDenied`; the status in the line says which). `<path>` is what the component asked for, relative to its /svc, through `ioservice::printable_path()`: a component can send any bytes in a name, a newline among them, and a line that quoted them as they came would let it write a line of the root's own, such as `routing_checks: passed`, which `boot_check.py` looks for.

`routing_checks` gets one small check to provoke it on every dev boot:

```cpp
auto check_undeclared() -> void {
    expect("a protocol the manifest does not use", connect_status(UNDECLARED), Status::NotFound);
}
```

`UNDECLARED` is `/svc/ours.test.Undeclared`, a name no manifest declares. `REQUIRED` in `boot_check.py` is unchanged: the check adds no line of its own when it passes.

**Reference**
None, this project's own design. The line is `log_route_error()`'s sibling for the refusal that happens before a route exists.

**Verification**
- `boot_check.py --out out/log-silent` with this tree: dbg PASS (44 passed, 4 skipped, 0 failed), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host tests PASS (151): the counts of the tree before. The rel log, all the root's failed-route lines after this change and the two earlier ones of this day:

```text
init: routing_checks's use of "denied" routes nowhere: AccessDenied at . (offer of "denied")
init: routing_checks's use of "ours.test.Unoffered" routes nowhere: NotFound at . (offer of "ours.test.Unoffered")
init: routing_checks's /svc refuses an open of "ours.test.Undeclared": NotFound
init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)
```

- /boot stays silent, in the same boot: `routing_checks` asks `fsio::spawn()` for `/boot/bin/no_such_program`, which the /boot connection refuses with NotFound, and the log has no line with `no_such_program` in it. Breaking the classification on purpose shows what it keeps out: with `first_service()` answering a value for every directory, the same boot prints, besides the line above, `init: .'s /svc refuses an open of "bin/no_such_program": NotFound` (twice) and `... of "bin/routing_checks": AccessDenied` and `... of "meta/routing_checks.cm": AccessDenied`, all /boot refusals under the root's moniker. Put back, the boot above has the one line.
- No host test: the root needs the kernel. The classification rests on `Tree::first_child()` and `Tree::service()`, which have tests, and the hook it hangs on has its own (`ioservice.md`, same day).
- clang-tidy against `out/log-silent`, `--header-filter` on `system/service/init`, on `realm.cpp`, `boot_server.cpp` and `routing_checks.cpp`: no diagnostic. `format_cpp.py --check` and `lint_docs.py --changed`: clean.

**Deliberately not done**
- No throttle, as for `log_route_error()`: it logs every failed route unthrottled. A component can make the root print one line per Open it sends, and can already with a use that routes nowhere; each line is bounded (the path is cut at 128 bytes), allocates nothing, and the rest of the root's work is not waiting on it. A rate limit would need a clock the root's `Realm` does not hold, and would hide the first line of a real typo behind a counter.
- A component with no protocol `use` is not reported, for the reason in the Symptom.
- No line for a refused Open on a namespace entry that is not /svc. A directory `use` whose Open fails is the component reading an epitaph the root already logged when it routed the entry (`cannot be served`, `routes nowhere`), or a file that is not there.
- The guide's chapter 5 (causes 2 and 4, the note "which is a known limit", the four questions) is not edited here: it describes the root as silent for these two cases.

**How it was found**
From the maintainer's decision of 2026-10-30 to log the three silent NotFound cases; cause 2 of the guide's table was the first of them.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The root serves /boot and every /svc with one `LoopServer`, and the hook is called for all of them. What in the tree tells a /svc from a /boot directory, why can a /boot directory never be mistaken for one, and what does the key give besides the answer?
2. A component with no protocol `use` opens `/svc/x`. Why does the root print nothing, and which side answers?
3. A component names a file in `/svc` with a newline and `routing_checks: passed` in it. What does the root's line show, what would it have shown without `printable_path()`, and which check of the boot would that have fooled?
