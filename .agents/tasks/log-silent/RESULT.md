# log-silent: three silent NotFound cases of a protocol connect now log a line

**Date**: 2026-10-30

## Delivered
The maintainer's decision of 2026-10-30, as three commits on the branch `log-silent` (worktree `.worktrees/log-silent`), one case each, each built and booted on both images before it was made:

1. `dee15336` `feat(ioservice): a refused Open of an unserved name is reported` (L3, the provider's line).
2. `2abd6f8c` `feat(init): the root logs a route to a provider that has exited` (L2).
3. The commit that adds this file, `feat(init): the root logs an Open of a /svc name the component does not use` (L1).

The three lines, as they appear on a real boot:

```text
ioservice: the outgoing directory refused an Open of "svc/ours.examples.echo.Echo": NotFound
init: routing_checks's use of "ours.test.Echo" routes nowhere: NotFound at echo (exited)
init: routing_checks's /svc refuses an open of "ours.test.Undeclared": NotFound
```

The first was seen on a scratch worktree whose echo passes `"ours.examples.echo.Echoo"` to `add_protocol()` (not committed); the other two are in every dev boot, the second from the existing `routing_checks` and echo's exit, the third from a new `check_undeclared()` beside `check_unoffered()`.

The design, in one paragraph each:

- The core does not print. `Outcome` of a `Refuse` carries the status and, for an Open, the path asked for. `LoopServer::Delegate` gets `open_refused(directory, path, status)`, virtual with an empty default beside `write_failed()`, called after the client has its epitaph. Nothing is called when no one listens, and a refused Clone is not reported.
- `Outgoing` overrides it and writes the line with the vDSO's `debug_write`, as it already calls `handle_close`. No program sets anything up, so `examples/echo`, `examples/hello` and the developer's guide are unchanged, and the library gains no dependency on `ort` (a user-only target; the host tests of `Outgoing` could not link it). The fake kernel defines `debug_write`, so the line is checked on the host.
- The root's `Realm` overrides it too and reports only a refusal on a component's /svc. The hook gives it the directory; a directory whose first child is a service is a /svc, and the key of that service names the instance (`BootServer::first_service()`). /boot and the subdirectories components hold have no service, so their refusals, a file that is not there, stay silent.
- `printable_path()` (in `tree.hpp`) puts a client's path in a line: bytes outside printable ASCII become `?`, a long path is cut and ends in `...`. A path is bytes the client chose, a newline included, and a line that quoted it could forge a line of the root's own, such as `routing_checks: passed`.
- No throttle is added, for the same reason `log_route_error()` has none; the change records say so.

## Verification
- Host tests, from `out/log-silent`: `ioservice/test/ioservice` 48 passed (42 before), `loop_server` 21 (15), `outgoing` 14 (9). Nine deliberate breakages of the L3 code, each caught (a missing call, a missing null-delegate check, a reported Clone, a report before the answer, an unsanitised path, the wrong directory, an ellipsis off by one, a dropped status, a wrong printable range); the list is in `docs/changes/261029/ioservice.md`.
- `boot_check.py --out out/log-silent`, run before each commit and on the final tree, the same each time and the same as before the work: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host tests PASS (151). `REQUIRED` in `boot_check.py` is unchanged.
- L2: three dev boots, one line each, after `init: echo exited with 0`. L1: the line in the dev boot; none of the `/boot` refusals the same boot makes is logged (`bin/no_such_program`), and with the classification broken on purpose they are (a scratch mutation, put back).
- clang-tidy against `out/log-silent` on every changed C++ file (the library sources and tests, `realm.cpp`, `boot_server.cpp`, `routing_checks.cpp`): no diagnostic. `format_cpp.py --check` and `lint_docs.py --changed`: clean.
- Not verified: the second wording of L2 (`its outgoing directory is closed`), which needs the root to handle a route between the PEER_CLOSED on echo's outgoing directory and the TERMINATED of its process; every boot saw `exited`. The line on the target for a provider other than echo with a typo, which was provoked in a scratch copy only. The root's L1 line for a status other than NotFound (host tests cover the hook for several).

## Deviations from the design
None; there was no design document, and the brief is the specification. Two additions to it are listed under "Found along the way".

## Found along the way
- Log forging. A path in a refused Open is the client's bytes, and a name may hold any byte but `/` and NUL, so the first version of L1 and L3 would have let a component write lines of the root's console, and `routing_checks: passed` is a line `boot_check.py` requires. `printable_path()` is the fix, with host tests; it is an addition to the brief, and the one place the brief's "must not make the console a way to hurt the root" asked for more than the absence of a throttle.
- The comment above `LoopServer::Delegate` named `BootServer` as its user in the root; it is `Realm`. Corrected in the lines already being changed.
- `.agents/tasks/directory-capability/DESIGN.md` (lines 289 and 341) cites `docs/repairs/261029/component-manifest.md` item 1 as a live item; it is fixed and the file is deleted. Not edited: it belongs to another task.
- The change records are in `docs/changes/261029/` as the brief says, with the date of the decision, 2026-10-30, in each title; `docs/changes/261030/` exists for other modules.

## Not done
- The developer's guide is not edited, as the brief says. Chapter 5, "What went wrong: what a client sees", is now wrong in these places:
  - The table row "an epitaph `NotFound` | nothing for this connect | 2, 4, 5": cause 2 now has the root's line `init: <moniker>'s /svc refuses an open of "<name>": NotFound`; cause 4 has `init: <moniker>'s use of "<name>" routes nowhere: NotFound at <provider> (exited)`, and its sentence "The root prints nothing for the route itself, which is a known limit" is false; cause 5 has the provider's own line `ioservice: the outgoing directory refused an Open of "svc/<name>": NotFound` and the root's log is still silent.
  - "Four questions, in order", question 4 ("If the root's log is silent ... causes 2, 4, 5, 10 and 13") and question 3, which searches the root's log for `use of "<name>"` only.
  - The note on a hand-built Open (`InvArgs`, `AccessDenied` on a /svc): the root now logs those too, as `refuses an open`.
  - "Other lines the root prints" has two more shapes. Causes 1, 3 and 6 to 13 are unchanged.
  - Chapter 4, which lists `write_failed()` as the delegate's one other function, and its line references.
- The provider's line does not name the program: the library does not know it and the debug console adds no prefix. A setter on `Outgoing` and a line in `echo` and `hello` would add it. Not done: the brief asked for the guide and the examples to stay as they are.
- A refused Clone is not reported (no path to name; its cause is a full connection table).
- No throttle, in any of the three.
