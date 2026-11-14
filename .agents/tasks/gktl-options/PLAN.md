# gktl-options: declarative options for console commands

**Date**: 2026-11-14
**Status**: Accepted

## Request

The user (2026-11-14): GKTL_COMMAND should let a command declare its options,
cxxopts-style, so no command hand-parses arguments again.

## Decisions

| Decision | Options | User's choice |
|---|---|---|
| Declaration shape | schema struct / X-macro table / runtime builder | **runtime builder** — construct, add, parse, the literal cxxopts shape |
| Migration scope | exemplar only / machinery only / all 12 commands | **all 12 commands** |
| Verb grammar | keep word forms / reshape verbs into options | **verbs become options** — the whole tree speaks one `name`/`name=value` grammar |

Framework choices (set in the plan, approved with it): builder lives in
`kernel/lib/gktl`, header-inline so host and kernel compile one definition;
no heap — a fixed 16-slot descriptor table and a 16-slot value table, sized to
the console's 16-word cap; reuses the repo's grammar semantics (bare name =
true, strict `1/true/yes` vs `0/false/no` bools, digits-only overflow-checked
numbers — `parse_u64` promoted from ktest to gktl); `Result`-shaped rejection
naming the fault and the offending word; unknown words rejected unless the
command declares `pass_through()`; duplicate names: last wins; error lines
pinned as `<cmd>: ERROR ...` and usage lines generated from the declarations;
the `GKTL_COMMAND` macro itself unchanged — declaration happens in the handler
body.

## Steps

Each step lands as one commit with its host tests green, `boot_check.py`
dbg+rel+host green, and cov_host ≥95% lines on the touched units.

1. Builder core + host suite: `gktl/options.hpp` (descriptors, parse, Parsed,
   usage generation, pinned error lines), the shared digit and bool
   primitives, the new `gktl-test` host target, ktest re-uses `gktl::parse_u64`.
2. Migrate the benches and the simple commands: steal.bench,
   preemptiondeadline.bench, largepage.bench (bench::Run adapted), crash, cov,
   idle.stats, steal.policy — the five hand-rolled digit parsers die here; old
   word forms swept from scripts and docs.
3. Migrate cpu, lockup, log: verbs become options; log's argc-overloaded
   grammar retires, the verb-dependent required checks stay in the handler.
4. Migrate ktest: `sel=` grammar plus runner options plus pass-through; every
   pinned `ktest: ERROR` line kept verbatim; `test_kshell_run.py` cases and
   the design doc's grammar block updated; the 83-test run regressed.
5. Close out: change record (naming the superseded ktest grammar decision),
   RESULT, the console's silent 16th-word truncation filed to `docs/repairs/`.

## New grammar per command

| Command | Today | Becomes |
|---|---|---|
| crash | `crash panic\|panic-locked\|panic-smp` | `crash how=<panic\|panic-locked\|panic-smp>` |
| cov | `cov dump` | `cov verb=dump` |
| cpu | verb + hand parse | `cpu verb=<...> cpu=<N>` (names fixed when read) |
| lockup | `status\|nmi\|spin <seconds>` | `lockup verb=<...> [seconds=N]` |
| log | argc overloading | `log verb=<level\|get> [domain=...] [level=...]` |
| idle.stats | positional seconds | `idle.stats [seconds=N]` |
| steal.policy | positional name value | `steal.policy name=<knob> value=<N>` |
| steal.bench | `rounds=N` + all | `steal.bench [all] [rounds=N] [csv]` |
| preemptiondeadline.bench | `all` + csv peek | `preemptiondeadline.bench [all] [csv]` |
| largepage.bench | `all` | `largepage.bench [all]` |
| dump-frame | ignores argv | not migrated — nothing to parse |
| ktest | `all\|list\|?\|death <f> [opts]` | `ktest sel=<word> [filter=<glob>] [shuffle[=SEED]] [repeat=N] [shard=M/N] [passthrough...]` |

The ktest row supersedes the settled grammar decision of 2026-11-05/07
(`docs/design/261105/kernel-test-framework.md:139-154`); the change record
says so explicitly.

## Files

- `kernel/lib/gktl/**` — the builder, its host suite
- `kernel/commands/*.cpp`, `benches/kernel/**` — the migrations
- `kernel/lib/ktest/**` — parse_u64 re-use, then the ktest migration
- `scripts/test_kshell_run.py` — grammar cases
- `docs/design/261105/kernel-test-framework.md` — grammar block superseded
- `.agents/tasks/gktl-options/` — this plan, RESULT

## Test plan

Per step: gktl-test (new) and ktest-test (83) green on host; cov_host ≥95%
lines on touched units; boot_check dbg+rel+host; kshell_run drives each new
word form on target (`crash how=panic`, `steal.bench all csv`,
`ktest sel=all shuffle repeat=2 shard=0/1`); `test_kshell_run.py` green; every
pinned ERROR/usage line has a host case.
