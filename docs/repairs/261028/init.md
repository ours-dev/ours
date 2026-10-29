# init outstanding repairs

## 1. The root's built-in protocols are matched by name twice, in `BUILTINS` and again in `Realm::connect()`

**Severity**
Low: nothing is wrong today. It is a maintenance trap, with a cost that is not the problem.

**Symptom**
Latent, cannot be triggered today. Every protocol in `BUILTINS` (`system/service/init/realm.cpp:51`) has a branch in the `if` chain of `Realm::connect()` that picks its server (`realm.cpp:746-758`), so every route to a built-in ends in a `bind()`. It would be triggered by a built-in protocol added to `BUILTINS` without a branch: `component::route()` finds it, `connect()` matches no name, `status` stays `NotFound`, the root logs `init: cannot serve <name> to <moniker>: NotFound`, and the client's channel is closed with the epitaph NotFound. That reads as "not found" for a route that was found, and nothing at build time or in a test says a branch is missing.

**Cause**
One mapping, from a built-in's name to the server that answers it, is written in two places that are matched by name. `route()` already finds the built-in by name in `BUILTINS` (`detail::find_builtin()`), but returns only `Route::name`, the string. `connect()` then compares that string against each protocol's name again, to learn which of the root's servers (`m_builtins.launcher`, `debug_resource`, and so on) to bind. Nothing ties the two lists together, so the compiler cannot notice that one has an entry the other lacks.

The cost of the second match is not the cause. It is at most six comparisons of names of 20 to 30 bytes, made once when a connection is made and not per message, which is a small fraction of one system call (a `write(2)` takes 580 ns on the host, `docs/perf/261028/loop-dispatch.md`). The user asked on 2026-10-28 whether it should be pre-hashed; a hash would make the second match faster and leave the second list in place, so it is ruled out below.

**Change**
Make the route say which built-in it ended at, so that the second match disappears:

- Give `component::Builtin` an `id` the caller chooses, and `component::Route` the id of the built-in it ended at, in place of nothing (a provider route has none). `route()` already holds the `Builtin` it found, so this is one field copied.
- In `Realm`, an `enum class BuiltinId` for the six protocols and `boot`, set in `BUILTINS`, and a `switch (routed->builtin)` with no `default`, so that `-Wswitch` under `-Werror` makes a built-in with no case a build error.

Ruled out: a pre-computed hash of the name, which speeds up a comparison that is not slow and keeps two lists; a table of server pointers parallel to `BUILTINS` and indexed by position, which turns a missing entry into a wrong one when the two are reordered, where the enum and the `switch` make it a compile error.

The change touches the `component` library's interface (`Builtin` and `Route`), whose tests in `system/lib/component/test` build `Builtin` entries and must be given an id.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "routed->name ==" system/service/init/realm.cpp` finds nothing. Adding a protocol to `BUILTINS` without a case in the `switch` fails the build. The `component` host tests pass, and `python3 .agents/skills/ours-verify/scripts/boot_check.py` passes both images with `routing_checks: passed`, which connects to the Launcher and the resource protocols through this path.
