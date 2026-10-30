# loop-waiter-realm: a component instance's two waits are the Loop's Waiters

**Date**: 2026-10-29

## Delivered
Step 5 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-realm` (its hash is in the report that handed this step back, since a commit cannot name itself), `refactor(init): a component instance's two waits are the Loop's Waiters`, on top of `ff090c31`:

- `Instance` in `system/service/init/realm.hpp` has two private nested classes, `OutgoingWaiter` and `TerminatedWaiter`, each `final: public loop::Waiter` with a private `on_signals()` override and a back pointer to its `Instance`; two members of them; and a private `attach(Realm &)` that sets `m_realm` and both back pointers. `Realm::on_outgoing` and `Realm::on_terminated` are private non-static members that take `Instance &`; the static `on_*(void *, Signals)` are gone.
- `system/service/init/realm.cpp`: the two `on_signals()` bodies hand the call to the Realm; `resolve_root()` and `resolve()` call `attach()` right after the `= Instance{}` reset, where they used to set `m_realm`; the two `m_loop->add()` calls pass `instance.m_outgoing_waiter` and `instance.m_terminated_waiter`; the handlers keep their bodies.
- Every comment that named the callbacks, the `void *` context or the slot's callback names the waiters or `on_signals()`.
- `docs/changes/261029/init.md` (a section appended), this workspace.

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything. Host tests, from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 13, `host/obj/system/service/init/test/autorun` 5, all with 0 failed.
- `grep -rn "Callback\b" system examples --include='*.cpp' --include='*.hpp'` finds only the Loop's own definitions (`loop.hpp`: the comment, the alias, `Slot::m_callback`, the old `add()`; `loop.cpp`: the old `add()`) and one test name in `loop_test.cpp`.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/system/service/init/.*'`, on `realm.cpp`: exit 0, no diagnostic. `format_cpp.py --check`: clean. `lint_docs.py --changed` on each document: no violations.
- `boot_check.py --out out/lw-realm` with the final tree: dbg PASS (shell finished at 5.5 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.6 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, `routing_checks: passed`, `init: echo exited with 0`, `pc-ps2: key a` and `pc-ps2: key A`); host tests PASS (148 passed, 0 failed). Both logs hold `shell: loop checks passed`.
- Deliberate breakages, the source put back after each (`cmp` against a saved copy): `TerminatedWaiter::on_signals()` answering `Close` without the Realm's handler: rel FAIL, `no "init: echo exited with 0" within 60s`, 5/6 required lines. `OutgoingWaiter::on_signals()` answering `Close` without clearing `m_outgoing`: rel PASS, 6/6; the boot does not depend on the clearing, because `Realm::forward()` also tests `m_state == Exited` (see the change record). `OutgoingWaiter::on_signals()` trapping: rel FAIL, 6 FAILED lines, 4/6 required, so the outgoing wait is on the boot path.
- `sizeof(Instance)` 224 to 256 bytes (two waiters of 16: vptr and back pointer); `INSTANCES` 1792 to 2048 bytes, type `b` before and after. `init`: `.data` 51360 unchanged, `.bss` 56448 to 56704, `.text` 108846 to 109278 (432 bytes, 310 of them in `std::array<Instance, 8>::array()`, which stores the vptrs), `.data.rel.ro` and `.rela.dyn` 48 bytes each (two vtables), stripped file 177,648 to 178,176. Nothing moved from `.bss` to `.data`: `INSTANCES` is a `LazyInit`, built at run time.
- Not measured: the time of one wait's dispatch before and after, and anything on x86-64 hardware.

## Deviations from the design
The brief set the back pointers "where `m_realm` is set". They are set there, but through a new private `Instance::attach(Realm &)` that sets `m_realm` and both pointers, not by field writes: the nested classes' pointers are private, and the reset `= Instance{}` copies the waiters' fields, so the pointers cannot be set by a constructor. The second breakage of the brief did not fail the boot; a third, a trap, was added to show the path is on the boot path. Both are in the change record.

## Found along the way
- No boot check covers `OutgoingWaiter` clearing `m_outgoing`: `Realm::forward()` answers NotFound on `m_state == Exited` as well, and `check_echo()` allows one PeerClosed retry. The clearing matters for a provider whose outgoing directory closes while its process keeps running, or for the window between the two packets of a dying provider. Not recorded as a repair: the code is right and the gap is in what the boot exercises.
- `Realm::on_outgoing`, `on_terminated` and `forward` use no `Realm` state; `readability-convert-member-functions-to-static` would name all three and the repository does not enable it. Not changed.

## Not done
- Step 6 of `.agents/tasks/loop-waiter/PLAN.md`, the removal of `Callback` and the old `add()`: it waits for the user's go-ahead.
