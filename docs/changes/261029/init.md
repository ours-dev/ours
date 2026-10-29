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
None, this project's own design. Fuchsia's component_manager registers its built-in protocols in code, in `BuiltinEnvironment::new()` with `add_builtin_protocol_if_enabled()` (`src/sys/component_manager/src/builtin_environment.rs:596-597`, `:634-648`, as read in `docs/changes/261005/init.md`); ours keeps them in a data file because two programs, the root and the manifest compiler, need the same list (`.agents/tasks/protocol-identity/DESIGN.md`, decision 3).

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
