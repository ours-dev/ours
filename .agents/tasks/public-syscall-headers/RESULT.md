# public-syscall-headers: one file per domain, a public surface that is only the interface

**Date**: 2026-11-03

## Delivered

Four commits on `exp/public-syscalls` (worktree `.worktrees/exp-public-syscalls`, based on baseline/20260824 at 19bc16ae):

- `0b02c363` — the description split: `ours/syscalls/<category>.gni` (17 files, 62 calls), entries carry `category`; `syscalls.gni` imports and concatenates; `gen_syscalls.py` requires the key.
- `568f656b` — per-category generated declarations: `vdso_decls/<category>.inc`; `system/public/BUILD.gn` declares one output per category.
- `3f20c57f` — one public header per domain (12 new, 5 extended), each self-contained with its option words, limits and generated declarations; `syscall.hpp` keeps only `INSTANT_INFINITE` and `MAX_NAME_LEN`; ~40 includers updated.
- `a84831f2` — the surface cleanup: `panic.hpp` to `kernel/include/ours/`, `assert.hpp` self-sufficient down to the declared `vpanic` hook; `syscall/time.hpp` to `kernel/include/ours/chrono.hpp` (17 includes); `syscall/resource.hpp` to `ours/resource.hpp`, MIT, listed; `config/annotates.hpp` deleted; `public_headers.gni` lists exactly the directory.

## Verification

- `boot_check.py` after each commit, final state: build ok; dbg 45 passed / 5 skipped, rel 43 passed / 7-of-7 required, both `reached: end`; host tests 159 passed / 0 failed.
- The vDSO's exports are checked against `libours.ifs` during the build; it passes with the renumbered list.
- Generated `numbers.hpp`: `SYSCALL_COUNT = 62`, dense per-category numbering.
- Grep guards: no reference to `ours/syscall/time.hpp`, `ours/syscall/resource.hpp`, `ours/config/annotates.hpp` or a public `ours/panic.hpp` remains; `public_headers.gni` diff-checked against the directory (exact match).
- clang-format (`format_cpp.py --check`) clean on all changed C++ files; clang-tidy run on TUs covering the new headers (`vmo.cpp`, `task.cpp`) — no diagnostics on changed lines, only pre-existing ones elsewhere.

## Deviations from the design

- **Renumbering.** DESIGN/PLAN first said "import order = today's entry order, numbers unchanged". The original order interleaves domains (vmo in four runs), so contiguous categories cannot preserve it. Numbers were renumbered, which the design comment in `syscalls.gni` explicitly allows (private to kernel and vDSO; both regenerate). The vDSO's symbols and signatures are untouched.
- **`all.inc` came and went.** Step 2 generated an umbrella `all.inc`; step 3 dropped it — the domain headers themselves are the umbrella's parts, and an umbrella over them would redeclare every call.
- **No separate `thread.hpp`.** The thread calls are declared in `<ours/task.hpp>`: the kernel defines them in `task.cpp`, and a category is named for the kernel file. Zircon splits thread/task; ours follows its own file boundary.
- **`chrono.hpp`, not `time.hpp`.** Step 3 had already taken `<ours/time.hpp>` for the clock syscalls' header, and kernel/include and system/public share the kernel toolchain's include path — a second `ours/time.hpp` would shadow by include order.
- **The assert/panic seam sits at PanicSink.** The plan moved only the declarations; reality: `panic_assertion` (a template) and `panic()` both render through PanicSink/`detail::render`, so the whole rendering front end moved into `assert.hpp`, ending in the declared `PHYS_EXPORT vpanic(std::string_view)` hook; kernel `panic.hpp` keeps the other vpanic overload and the panic state.

## Found along the way

- `PHYS_EXPORT` mismatch: declaring `vpanic` without it in `assert.hpp` while `panic.hpp` had it made the same function two declarations with different visibility in kernel/phys TUs ("visibility does not match previous declaration"). Fixed by giving the public declaration the attribute.
- `wire.hpp` and `process_args.hpp` used `CHANNEL_MAX_*` from `syscall.hpp`; the carve-out surfaced them (and one `fsio.cpp` unused-field cascade that was the same root cause in disguise).
- Stale generated files in `out/` (a `vdso_decls/` without the `ours/` segment from a failed attempt) satisfied nothing but confused a `find`; removed by regenerating.

## Not done

- No sysroot generation step (the list is its source of truth, but nothing consumes it yet).
- `ours/version.hpp` and the four clock/version calls (`ticks_per_second`, `boot_offset`, `published_version`, `kernel_version`) exist only as uncommitted work of the vdso-published-data session in the main checkout; this branch is based on HEAD without them. When that lands, its entries go into `ours/syscalls/time.gni` and `version.hpp` joins `public_headers.gni` — a small, bounded merge.
