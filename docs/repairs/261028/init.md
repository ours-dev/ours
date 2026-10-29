# init outstanding repairs

## 1. `Realm::connect()` chooses a built-in's server by comparing names, and nothing says a listed built-in has one

**Severity**
Low: nothing is wrong today. It is a maintenance trap, with a cost that is not the problem.

**Symptom**
Latent, cannot be triggered today. Every protocol in `system/service/init/builtins.json` has a branch in the `if` chain of `Realm::connect()` that picks its server (`system/service/init/realm.cpp:757-768`), so every route to a built-in ends in a `bind()`. It would be triggered by a protocol added to `builtins.json` without a branch: `component::route()` finds it, the manifest compiler accepts a route that ends at it, `connect()` matches no name, `status` stays `NotFound`, the root logs `init: cannot serve <name> to <moniker>: NotFound`, and the client's channel is closed with the epitaph NotFound. That reads as "not found" for a route that was found, and nothing at build time or in a test says a branch is missing.

**Cause**
The list is one list now. `BUILTINS` in `realm.cpp` is generated from `builtins.json`, the file the manifest compiler also reads, and a `static_assert` for each name a library defines (`ours_process::LAUNCHER_NAME` and the five `ours_resource` protocols) stops the build if the list drops one (change record `docs/changes/261029/init.md`). What is left is the other half: the mapping from a built-in to the server that answers it is a second thing, written in `connect()` as a chain of comparisons of `routed->name` with the library's constants, and the `static_assert`s tie each of those constants to the list in one direction only. A name that is in the list and in no branch compiles. `route()` returns only `Route::name`, the string, so `connect()` has nothing but the name to choose with.

The cost of the comparisons is not the cause. It is at most six comparisons of names of 20 to 30 bytes, made once when a connection is made and not per message, which is a small fraction of one system call (a `write(2)` takes 580 ns on the host, `docs/perf/261028/loop-dispatch.md`). The user asked on 2026-10-28 whether it should be pre-hashed; a hash would make the comparison faster and leave the chain in place, so it is ruled out below.

**Change**
Make the route say which built-in it ended at, so that the chain disappears:

- Give `component::Builtin` an `id` the caller chooses, and `component::Route` the id of the built-in it ended at (a provider route has none). `route()` already holds the `Builtin` it found, so this is one field copied. `builtins.json` gains the id, so that the generated list carries it and the manifest compiler ignores it.
- In `Realm`, an `enum class BuiltinId` for the six protocols and `boot`, named by the generated list, and a `switch (routed->builtin)` with no `default`, so that `-Wswitch` under `-Werror` makes a built-in with no case a build error. The `static_assert`s on library names then have nothing left to do and go.

Ruled out: a pre-computed hash of the name, which speeds up a comparison that is not slow and keeps the chain; a table of server pointers parallel to `BUILTINS` and indexed by position, which turns a missing entry into a wrong one when the two are reordered, where the enum and the `switch` make it a compile error.

The change touches the `component` library's interface (`Builtin` and `Route`), whose tests in `system/lib/component/test` build `Builtin` entries and must be given an id, and the data file's format, whose reader is `system/component_builtins.py`.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "routed->name ==" system/service/init/realm.cpp` finds nothing. Adding a protocol to `builtins.json` without a case in the `switch` fails the build. The `component` host tests pass, and `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes both images with `routing_checks: passed`, which connects to the Launcher and the resource protocols through this path.
