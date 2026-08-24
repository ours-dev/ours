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
flags, exact place they live (only inside the `dev-common` container, same
as `gn`/`ninja` — see below).

## Where the tools live

`clang-format` and `clang-tidy` are **not** on `PATH` in a fresh shell inside
the container — use the full path, same as this repo's `clang++`:

```
/usr/lib/llvm-22/bin/clang-format
/usr/lib/llvm-22/bin/clang-tidy
```

Run them via `docker exec dev-common bash -lc '...'` (or `export PATH=` first
if running several commands in one session) — there is no host-side install
these should be assumed to use; this project's whole toolchain lives in the
`dev-common` container.

## Step 1 — clang-format

```bash
docker exec dev-common bash -lc \
  '/usr/lib/llvm-22/bin/clang-format -i <repo-path-to-file>'
```

Run from the repo root inside the container (paths are relative to wherever
the repo is mounted, `/home/developer/workspaces/projects/.../ours.refactor.reasonix`
in this setup). `-i` formats in place. Uses `.clang-format` at the repo root
automatically — no need to pass `-style=file` explicitly, clang-format finds
it by walking up from the file.

Do this for every file you wrote or edited, not just ones you're unsure
about — formatting drift is easy to introduce by hand and cheap to fix here.

## Step 2 — clang-tidy

Prefer pointing clang-tidy at the build's `compile_commands.json` instead of
hand-assembling include flags — this repo already generates one on every
`gn gen` (`.gn` sets `export_compile_commands`), so the real flags for any
given target (`-std=c++23`, the target's actual `-I`s, `-D`s) are already
there and don't need to be guessed:

```bash
docker exec dev-common bash -lc \
  '/usr/lib/llvm-22/bin/clang-tidy -p out/obi-check <repo-path-to-file>'
```

(Substitute whatever GN output directory is live — `out/obi-check` is this
session's, not a fixed name. If the file isn't part of that build's target
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

- [ ] `clang-format -i` was run on every `.hpp`/`.h`/`.cpp`/`.cc`/`.cxx` file
      touched, and the diff shows no leftover formatting-only hunks
- [ ] `clang-tidy` was run on every touched file against a build directory
      that actually covers it
- [ ] Every diagnostic clang-tidy reported on changed lines is either fixed
      or has a narrow `// NOLINT(<check>)` with a stated reason
- [ ] Nothing was suppressed repo-wide (`.clang-tidy` itself) just to make a
      single finding go away
