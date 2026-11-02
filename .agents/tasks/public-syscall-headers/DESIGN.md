# public-syscall-headers: system/public keeps only the userspace system interface, and syscalls live per domain

**Date**: 2026-11-03
**Status**: Accepted

## Problem

Two things in `system/public` have drifted from what the directory claims to be
("public Ours headers, which the kernel and user programs both compile
against" — the future sysroot's source of truth):

1. **Headers that are not the system interface live there.** `ours/panic.hpp`
   declares a kernel/phys function (no user program can call it; Zircon keeps
   `panic.h` out of `system/public` — its public `zircon/assert.h` only
   declares a `__zx_panic` hook each environment defines). `ours/syscall/time.hpp`
   is included by 18 kernel files and no user file — it is the kernel's
   time vocabulary, not a syscall header, and it sits on the would-be sysroot
   path. `ours/config/annotates.hpp` is an empty shell with one includer.
   `ours/syscall/time.hpp` and `ours/syscall/resource.hpp` are GPL-licensed
   while the rest of the public surface is MIT.
   Six of the 24 headers are not in `public_headers.gni`
   (`clock_page.hpp`, `config/annotates.hpp`, `syscall/time.hpp`,
   `syscall/resource.hpp`, `time_values.hpp`, `version.hpp`), so the list no
   longer describes the directory.

2. **The syscall surface is centralised where the rest of the system is
   per-domain.** `kernel/lib/syscall/src/` has one file per domain
   (`vmo.cpp`, `channel.cpp`, ...), but the description is one 2276-line
   `syscalls.gni` and the generated declarations are one flat
   `vdso_decls.inc` that `<ours/vdso.hpp>` includes. A domain's ABI is spread
   over four places that do not name each other: the entry in `syscalls.gni`,
   its option words/limits (most sit in `ours/syscall.hpp` regardless of
   domain), its generated declarations inside the flat `.inc`, and its handler
   in `kernel/lib/syscall/src/<domain>.cpp`.

Zircon's settled shape for both problems
(`~/Workspaces/projects/fuchsia/zircon`):

- One description file per category: `zircon/vdso/channel.fidl`,
  `port.fidl`, `vmo.fidl`, ... (~30 files).
- Per-category public headers for a domain's types:
  `zircon/system/public/zircon/syscalls/port.h` holds the packet layout and
  the port calls' option words.
- An umbrella: `zircon/syscalls.h`.
- Shared compile-time headers stay public (`zircon/compiler.h`,
  `zircon/assert.h`); kernel-only facilities do not (no `panic.h`).

## Goals and non-goals

Goals:

- `system/public` holds only what a userspace program (or a future OS built on
  ours) compiles against, and `public_headers.gni` lists exactly that.
- One domain = one description file = one public header, in step with the
  kernel's own per-domain handler files.
- Call numbers do not change: the vDSO binary ABI is bit-identical after this.

Non-goals:

- No new syscalls, no changed signatures, no changed semantics.
- No sysroot generation step (the gni comment is aspirational; building one is
  out of scope).
- No touching the in-flight vdso-published-data work (`anchor_page`,
  `time_values.hpp` contents stay as they are on HEAD).

## Design

### Part 1 — what stays in system/public

| Header | Verdict | Reason |
|---|---|---|
| types, status, rights, signals, vdso, process_args, acpi_tables, port, exception, interrupt, bti, sampler, object_info, object_property | keep | The ABI itself; all have real user-side includers. |
| compiler.hpp, assert.hpp | keep | Zircon keeps both in `system/public/zircon/`. |
| clock_page.hpp, time_values.hpp, version.hpp | keep, add to list | Published data the user side reads (launcher, shell, lib/time); unlisted by omission. |
| syscall/resource.hpp | move to `ours/resource.hpp`, add to list | RsrcKind is genuine ABI (service/init, shell include it); it was never listed because it hid under `syscall/`. License needs the user's call (see Decisions). |
| panic.hpp | move to `kernel/include/ours/panic.hpp` | Kernel/phys function. 26 of its 27 includers are kernel-side; the last is `assert.hpp`. Zircon keeps panic out of the public tree. |
| assert.hpp | rework its failure path | Stops including `panic.hpp`; declares `ours::panic_assertion(...)` itself (the Zircon `assert.h` shape: the header declares, each environment defines — kernel from panic.hpp, a user runtime from its own). |
| syscall/time.hpp | move to `kernel/include/ours/time.hpp` | 18 kernel includers, zero user includers; ustl-chrono vocabulary of the kernel, misfiled as a syscall header. 18 include lines updated. |
| config/annotates.hpp | delete | Empty pass-through over compiler.hpp; one kernel includer moves to `<ours/compiler.hpp>`. |

`public_headers.gni` ends up listing every header the tree ships — one list,
no strays.

### Part 2 — syscalls per domain

**Description.** `ours/syscalls.gni` becomes a short file that imports one
file per category from `ours/syscalls/<category>.gni` and concatenates them
into `ours_syscalls`. The import order is today's entry order, so every call
keeps its number and nothing but the file boundaries changes. Categories are
the kernel's own file names — each entry is assigned to the
`kernel/lib/syscall/src/<domain>.cpp` that defines it, so the correspondence
is `ours/syscalls/vmo.gni` ↔ `kernel/lib/syscall/src/vmo.cpp` ↔
`ours/vmo.hpp`. Entries gain a `category` key (the generator currently
rejects unknown keys, so `build/abi/gen_syscalls.py` learns it in the same
step).

**Generated declarations.** The generator emits one
`public/ours/vdso_decls/<category>.inc` per category (replacing the flat
`vdso_decls.inc`) plus `public/ours/vdso_decls/all.inc`, which only includes
the per-category files. `<ours/vdso.hpp>` keeps its single
`#include <ours/vdso_decls/all.inc>`; a category's own header includes
`<ours/vdso_decls/<category>.inc>`. The four private outputs (numbers.hpp,
vdso_stubs.inc, kernel_decls.inc, kernel_shims.inc) stay flat: the numbers,
the stub list and the shim table are global by nature.

**Per-domain public headers.** A domain's option words and limits move out of
`ours/syscall.hpp` into that domain's header beside its generated
declarations — the port case already works this way (`ours/port.hpp` holds
the packet layout; it gains the port decls):

- new `ours/channel.hpp`, `ours/process.hpp`, `ours/thread.hpp`,
  `ours/task.hpp`, `ours/vmo.hpp`, `ours/vma.hpp`, `ours/handle.hpp`,
  `ours/object.hpp`, `ours/debug.hpp`, `ours/command.hpp`, `ours/ioport.hpp`,
  `ours/pager.hpp`, `ours/time.hpp` (the clock/time vDSO entries)
- existing `ours/port.hpp`, `ours/interrupt.hpp`, `ours/bti.hpp`,
  `ours/sampler.hpp`, `ours/exception.hpp` gain their domain's decls.
  `ours/object.hpp` includes signals/object_info/object_property and carries
  the object_* decls, as `zircon/syscalls/object.h` does.

`ours/syscall.hpp` keeps only the cross-domain constants (INSTANT_INFINITE,
MAX_NAME_LEN) — the shared-limits header, Zircon's `zircon/limits.h` slot.
`ours/vdso.hpp` stays the one-stop umbrella a program includes today; no user
program is forced to change.

**Counting the surface.** ~17 categories, ~13 new small headers, one gni
family, ~60 include-line updates across kernel/ and system/ for the carved
option enums — mechanical, compiler-checked.

## Alternatives considered

- **Keep `syscalls.gni` whole, only split the generated declarations.**
  Half the fix: the description stays one 2276-line file a domain cannot own
  a piece of, and the entry↔handler correspondence the user asked for stays
  implicit. Rejected.
- **Move compiler/assert/panic out of system/public entirely** (strict
  "nothing but ABI here"). Departs from Zircon, which keeps `compiler.h` and
  `assert.h` public as shared compile-time headers; the 12 system/lib
  includers of compiler.hpp would need a new include root for no gain.
  Rejected for compiler/assert; taken for panic, which is a kernel function,
  not a compile-time macro.
- **Flat description with a `category` key, no file split.** Same generation
  result, but the file stays centralised — the thing being fixed. Rejected.
- **Per-domain description files inside the kernel tree**, next to the
  handler .cpp. The description is the public ABI record; it belongs with the
  public headers it generates, not behind the trust boundary. Rejected.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope of the header carve-out | (a) full: descriptions, generated decls, option enums all per domain; (b) only descriptions + generated decls, `syscall.hpp` keeps all option words | (a) — it is the correspondence the request names; (b) leaves a domain's ABI in two places | — | (a), sharpened: a syscall and its parameter structures share one file, and a domain header is self-contained so a program includes just the domain it needs (2026-11-03) |
| panic.hpp / assert.hpp | (a) panic to kernel, assert declares its own hook (Zircon shape); (b) leave both as they are | (a) | — | (a) (2026-11-03) |
| `ours/syscall/resource.hpp` license | (a) relicense MIT with the move; (b) keep GPL-2.0 on the public ABI surface | (a) — user's own 2024 file, repo standard; a GPL header on the sysroot path would license the whole public surface into question | — | (a) MIT (2026-11-03) |
| Description file location | (a) `system/public/ours/syscalls/<category>.gni`, family under the existing root; (b) `system/public/syscalls/` outside the include dir | (a) — keeps the well-known path, .gni in an include dir is harmless | — | (a), taken as the default (2026-11-03) |

## Testing and verification

- `verify` (build + both images boot) after every step; in worktrees the
  skill is ours-verify (read SKILL.md directly if the Skill tool does not
  resolve it).
- The vDSO's exports are already checked against `libours.ifs`
  (`//kernel/lib/userabi/vdso:vdso.abi_check`), and `libours.ifs` is derived
  from the same list — a dropped or renamed call fails the build.
- Call-number stability is checked by construction (import order = today's
  order) and asserted by diffing the generated `numbers.hpp` against the
  pre-refactor one (must be byte-identical).
- grep guards: no include of `<ours/panic.hpp>`, `<ours/syscall/time.hpp>`,
  `<ours/config/annotates.hpp>` remains; `public_headers.gni` matches the
  directory contents.

## Open questions

- The in-flight vdso-published-data session has uncommitted changes to
  `build/abi/gen_syscalls.py` in the main checkout. This branch starts from
  HEAD (19bc16ae); whichever lands second rebases over a small textual
  conflict in that one file. Land this first if both are close.
- Where `MAX_PROCESS_READ_MEMORY` goes (process domain vs the shared limits
  header) — decide at implementation by the "one owner domain" rule.
