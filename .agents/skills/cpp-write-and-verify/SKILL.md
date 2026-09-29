---
name: cpp-write-and-verify
description: Mandatory workflow for writing or editing any C++ file in this repo (.hpp/.h/.cpp/.cc/.cxx) — follow the modern-cpp-development style rules, then run this repo's actual clang-format and clang-tidy against the file before calling the change done. Use whenever you create or edit a C++ file anywhere in this tree, not just in kernel/.
---

# C++ Write and Verify

Two things, every time you touch a `.hpp`/`.h`/`.cpp`/`.cc`/`.cxx` file in this
repo:

1. **Write it following `.agents/skills/language/modern-cpp-development`.**
   That skill has the actual style rules (const-first, no macros, no ternary,
   always-braced control flow, trailing return types, `enum class` only,
   etc.) — load it and follow it. Don't restate its rules here; this skill is
   the *enforcement* half, not a second copy of the style guide.

2. **Run this repo's real `clang-format`/`clang-tidy` before calling the
   change done — not a description of running them, the actual commands.**
   The generic skill says to do this; this skill exists because that step
   was being skipped in practice. A C++ edit isn't finished until both of
   these have run clean against the actual file.

## Why this exists as its own skill

`modern-cpp-development` already lists "verify with clang-tidy" and "format
with clang-format" as requirements (see its Requirements section, near the
bottom). In practice those two steps were the ones getting silently skipped
— C++ files got written and edited across a whole OBI build-system session
without either tool ever being invoked. This skill is the concrete,
repo-specific "how" so that doesn't happen again: exact binaries, exact
flags, exact place they live (the repo's own `prebuilt/` LLVM tree — see
below).

## Where the tools live

`clang-format` and `clang-tidy` live in the repo's prebuilt LLVM tree and run
natively on the host — no container, no install, use the full path:

```text
prebuilt/clang/llvm-23/bin/clang-format
prebuilt/clang/llvm-23/bin/clang-tidy
```

Find them with `ls prebuilt/clang/*/bin` if the tree moves: prefer the LLVM
that both ships these two tools and whose `clang++` the current build
compiles with, so clang-tidy checks exactly what the build compiles. Today
that's llvm-23 only (llvm-22 carries the compiler but not these tools), and
`out/<dir>/compile_commands.json` invokes
`../../prebuilt/clang/llvm-23/bin/clang++` — same tree.

If the prebuilt tree is absent, do **not** fall back to the `dev-common`
container, even if it is already running: the user has refused its use for
verification. Say plainly that formatting and lint could not be run, and never
report a host-side approximation as a clang-format/clang-tidy pass.

## Step 1 — clang-format

Formatting is automatic and covers only the lines you changed. In Claude Code
a PostToolUse hook (`.claude/settings.json`) runs `scripts/format_cpp.py` after
every Edit and Write, so there is nothing to remember; it tells you when it
reformatted something, and then you Read the file again before editing it
further. Any other agent, or a change made through Bash, runs it by hand:

```bash
python3 scripts/format_cpp.py <path-to-file>          # format your changes
python3 scripts/format_cpp.py --check <path-to-file>  # only report
```

It formats the lines that differ from HEAD, and a new file whole, using the
prebuilt `clang-format` and the repo's `.clang-format`. Do **not** run
`clang-format -i` on a whole existing file: about one C++ file in six here is
not clang-format clean, mostly under `system/lib`, so that buries a three-line
change in unrelated churn.

## Step 2 — clang-tidy

Prefer pointing clang-tidy at the build's `compile_commands.json` instead of
hand-assembling include flags — this repo already generates one on every
`gn gen` (`.gn` sets `export_compile_commands`), so the real flags for any
given target (`-std=c++23`, the target's actual `-I`s, `-D`s) are already
there and don't need to be guessed:

```bash
prebuilt/clang/llvm-23/bin/clang-tidy -p out/<gn-output-dir> <path-to-file>
```

(Substitute whatever GN output directory is live — `out/bringup.x86` is the
current one, not a fixed name. The compile database records host-relative
paths into this same prebuilt clang++, so `-p` resolves exactly from the
repo root. If the file isn't part of that build's target
graph, `-p` won't find flags for it; gen a build that reaches it first, or
fall back to passing flags by hand after `--` as the generic skill
describes.)

Every new diagnostic clang-tidy reports must be resolved before the change
is done: fix the underlying issue, or suppress narrowly with
`// NOLINT(<check-name>)` plus a one-line reason — never a bare `// NOLINT`
with no check name, and never disable a check repo-wide to silence one
finding. Pre-existing diagnostics in code you didn't touch are not your
problem to fix in the same change; new ones on lines you wrote or edited
are.

## Verification

Before considering a C++ change complete, confirm:

- [ ] The lines you changed in every `.hpp`/`.h`/`.cpp`/`.cc`/`.cxx` file are
      formatted (the hook did it, or `scripts/format_cpp.py --check` is quiet),
      and the diff shows no formatting-only hunks on lines you did not change
- [ ] `clang-tidy` was run on every touched file against a build directory
      that actually covers it
- [ ] Every diagnostic clang-tidy reported on changed lines is either fixed
      or has a narrow `// NOLINT(<check>)` with a stated reason
- [ ] Nothing was suppressed repo-wide (`.clang-tidy` itself) just to make a
      single finding go away
