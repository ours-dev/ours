# ktest-hsien-oop: ktest and hsien move from procedural to object-oriented code

**Date**: 2026-11-18
**Status**: Proposed

## Request

The user (2026-11-18): "refactor ktest and hsien. Use OOP not POP." Scope is `kernel/lib/ktest` and `system/lib/hsien`: state owned by objects with verb interfaces instead of free functions over globals and plain structs. Stateless helpers (`matches_filter`, `shuffle`, `in_shard`, `parse_*`) stay free functions, as `modern-cpp-development` allows. The console lines (the host's contract with `kshell_run.py`) do not change.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Starting point | continue the uncommitted half-finished refactor / restart from HEAD | continue | None | continue, 2026-11-18 |
| `hsien::Reporter` polymorphism | concept + template parameter / abstract base class with virtual `report()` / function pointer wrapped in a class | concept + template | None | abstract base class, 2026-11-18 |

## Steps

1. Finish the half-done refactor so it builds, passes and boots: revert the unfinished `Environment` rename, add `override` to test environments, drop the unused second `gktl::Options`. Verified by `boot_check.py` and `ktest all` on dbg. Commit `refactor(ktest): the runner becomes a Run object, records move into their classes`.
2. Fix the `sel=list`/`sel=death` word count (was `docs/repairs/261118/ktest.md` item 1). Commit `fix(ktest): sel=list and sel=death stop rejecting their own word`.
3. Remaining OOP steps, each introduced to the user before it starts: hsien `Reporter` as an abstract base class and a `TestRegistry` replacing `run`/`section_*`; ktest's published test state behind one `ActiveTest` object; run options as methods on an object instead of free functions over `ACTIVE`; `EnvironmentRegistry` as a class; the command as an object that builds a `RunPlan`. Whether `Environment` moves into hsien is decided with the first hsien step.

## Files

- Step 1: `kernel/lib/ktest/**`, `kernel/tests/ktest_fixture_test.cpp`, `docs/changes/261118/ktest.md`, `docs/repairs/261118/ktest.md`.

## Test plan

Each step: host `ktest-test` green, `boot_check.py` dbg and rel PASS with host tests, and on dbg `ktest all` and `ktest sel=all shuffle repeat=2 shard=0/2` with `failed=0`.
