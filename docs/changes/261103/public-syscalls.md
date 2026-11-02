# public-syscalls changes

## One file per domain: description, declarations and option words move together (2026-11-03)

**Files**

- `system/public/ours/syscalls.gni` — became the importer: one file per category under `ours/syscalls/`, concatenated in number order.
- `system/public/ours/syscalls/<category>.gni` (17 new) — each call with its category, named for the `kernel/lib/syscall/src/<category>.cpp` that defines it.
- `build/abi/gen_syscalls.py` — requires the `category` key; emits one `vdso_decls/<category>.inc` per category instead of the flat `vdso_decls.inc`.
- `system/public/BUILD.gn` — declares one generated output per category.
- `system/public/ours/{channel,command,debug,handle,ioport,object,pager,process,task,time,vma,vmo}.hpp` (new) — one header per domain: its option words and limits beside its generated declarations.
- `system/public/ours/{port,interrupt,bti,sampler,exception}.hpp` — gained their declarations.
- `system/public/ours/syscall.hpp` — keeps only `INSTANT_INFINITE` and `MAX_NAME_LEN`; every domain's words moved out.
- ~35 includers across `kernel/` and `system/` — one include line each.

**Symptom**

Latent, organisational: the kernel's handlers were already one file per domain, but a domain's ABI was spread over four places that do not name each other — its entry somewhere in a 2276-line `syscalls.gni`, its option words in `ours/syscall.hpp` regardless of domain, its declaration inside one flat generated `.inc`, its handler in `src/<domain>.cpp`. No header was self-contained, so no program could include just the domain it speaks.

**Cause**

The description had grown around the generator (one JSON list) instead of around the domains. `syscalls.gni` was built before the kernel's per-domain handler split and never followed it; `syscall.hpp` accreted every domain's option words because it predated all of them. The cost is real: adding a syscall touches files that do not refer to each other, and a reader of `<ours/vdso.hpp>` sees one undifferentiated list of 62 calls where the kernel itself sees 17 domains.

**Change**

The reference shape is Zircon's: one description file per category (`zircon/vdso/channel.fidl`, `port.fidl`, ...), per-category public headers holding that category's types and calls (`zircon/system/public/zircon/syscalls/port.h`), and an umbrella (`zircon/syscalls.h`). Ours now matches: `ours/syscalls/vmo.gni` ↔ `kernel/lib/syscall/src/vmo.cpp` ↔ `<ours/vmo.hpp>`, with `<ours/vdso.hpp>` as the umbrella. Before:

```text
syscalls.gni: 62 entries, all domains interleaved
vdso_decls.inc: one flat generated file
ours/syscall.hpp: VmOptions, VmoOptions, Channel*Options, ... all domains
```

after:

```text
ours/syscalls/vmo.gni      kernel/lib/syscall/src/vmo.cpp      ours/vmo.hpp
ours/syscalls/port.gni     kernel/lib/syscall/src/port.cpp     ours/port.hpp
... (17 categories)
```

The old order interleaved domains, so making each category contiguous renumbered every call. Renumbering is the designed-in freedom (design D8): numbers are private to the kernel and the vDSO, both regenerate from one list, and the vDSO's symbols and signatures — the only ABI a program sees — do not move. Rejected: keeping the old numbers by splitting categories into multiple files per domain (one category, many `.gni` files — the correspondence the refactor exists to create would be lost), and keeping `syscall.hpp` central with a `category` key only in the generator (half the fix; the description stays one file a domain cannot own a piece of).

**Reference**

`zircon/vdso/*.fidl` for the per-category description; `zircon/system/public/zircon/syscalls/port.h` for the domain header holding option words beside the calls; `zircon/syscalls.h` for the umbrella. Zircon's numbers have been renumbered repeatedly for exactly the reason used here: they are private (`zircon/syscalls/internal/cdecls.inc` is generated; no program sees a number).

**Verification**

`boot_check.py` after each commit: build ok, dbg 45 passed / rel 43 passed, host tests 159 passed. Generated `numbers.hpp` shows `SYSCALL_COUNT = 62` with dense per-category numbering; the vDSO's exports still match `libours.ifs` (checked as part of the build).

**Deliberately not done**

- No sysroot generation step; `public_headers.gni`'s comment stays aspirational.
- No new syscalls, no changed signatures, no changed semantics.

**How it was found**

None — a planned refactor, requested by the user.

**The user's prediction**

None — the user set the direction ("each syscall and its parameter structures in the same file; users include on demand") and approved the plan; nothing here was measured against a prediction.

**Three questions you should be able to answer**

- Why was renumbering the calls acceptable, and what would have broken if a program had hardcoded a number?
- Why does `pager.hpp` include `vmo.hpp`?
- Why do the thread calls live in `<ours/task.hpp>` and not a `<ours/thread.hpp>`?

## The public surface keeps only the system interface (2026-11-03)

**Files**

- `system/public/ours/panic.hpp` → `kernel/include/ours/panic.hpp` — the kernel's panic facility leaves the public tree.
- `system/public/ours/assert.hpp` — self-sufficient: the assertion front end (PanicSink, the rendering helpers, the two inline `panic_assertion` forms) moved in from panic.hpp; it ends in the declared hook `ours::vpanic(std::string_view)`.
- `system/public/ours/syscall/time.hpp` → `kernel/include/ours/chrono.hpp` — the kernel's Duration/TimePoint vocabulary, 17 kernel includes updated; named chrono, not time, because step 3's public clock header already took `<ours/time.hpp>` and the two include roots share the kernel toolchain's path.
- `system/public/ours/syscall/resource.hpp` → `system/public/ours/resource.hpp` — becomes a listed public header, relicensed GPL-2.0 → MIT.
- `system/public/ours/config/annotates.hpp` — deleted (empty pass-through; its one includer names `<ours/compiler.hpp>`).
- `system/public/public_headers.gni` — lists all 32 headers the directory ships, and nothing missing.

**Symptom**

Latent: `system/public` claimed to be "public Ours headers, which the kernel and user programs both compile against" — the future sysroot's source of truth — but held kernel-only facilities, and 6 of its 24 headers were not in `public_headers.gni`, so the list no longer described the directory.

**Cause**

`system/public` had accreted by convenience: `panic.hpp` sat there because `assert.hpp` needed it, `syscall/time.hpp` because its author needed a home for the kernel's time vocabulary, `syscall/resource.hpp` under `syscall/` because the directory layout suggested it. None of these is the system interface; Zircon's `system/public/zircon/` draws the line differently and deliberately: `compiler.h` and `assert.h` are public shared compile-time headers, `panic.h` is not public at all, and the failure hook is declared in the public header and defined per environment.

**Change**

`assert.hpp` and `panic.hpp` split at the hook. Before: `assert.hpp` included `panic.hpp`, which defined the assertion forms inline and chained to `vpanic` — a kernel/phys function, on the user-side include path only because nothing had moved it. After: `assert.hpp` declares the hook and defines the assertion front end; `panic.hpp` (kernel-side) keeps `vpanic`'s declarations, the panic state, and `panic()`; each environment defines the hook (kernel and phys in their panic.cpp files, the profdata host test in its own TU). The hook contract already existed in practice — profdata_test.cpp documented it — so the split follows the code's own seam rather than inventing one. `resource.hpp`'s relicense was the user's call: their own 2024 file, and a GPL header on the sysroot path would license the whole public surface into question.

**Reference**

`zircon/system/public/zircon/assert.h` — declares `__zx_panic` (user mode) and defers to the kernel's own assert under `_KERNEL`; no `zircon/panic.h` exists in the public tree. `zircon/system/public/zircon/compiler.h` — the shared compile-time header ours keeps too.

**Verification**

`boot_check.py`: build ok, dbg/rel pass, host tests pass. Grep guards: no references to `ours/panic.hpp` (public tree), `ours/syscall/time.hpp`, `ours/syscall/resource.hpp`, or `ours/config/annotates.hpp` remain; `public_headers.gni` matches the directory contents exactly (diff-checked).

**Deliberately not done**

- `compiler.hpp` and `assert.hpp` stay in `system/public`: Zircon keeps both public, and moving them would strand 12 system/lib includers behind a new include root for no gain.
- No sysroot step is built from the list yet.

**How it was found**

None — same planned refactor.

**The user's prediction**

None — the user approved each move (Zircon-shape assert/panic split; MIT relicense) before it was made.

**Three questions you should be able to answer**

- What does `<ours/assert.hpp>` end in, and who defines it in each environment?
- Why is the kernel's time vocabulary `kernel/include/ours/chrono.hpp` and not `kernel/include/ours/time.hpp`?
- Why do `compiler.hpp` and `assert.hpp` stay public while `panic.hpp` does not?

## WaitAsyncOptions moves beside object_wait_async (2026-11-03)

**Files**

- `system/public/ours/port.hpp` — the enum removed; the packet types and port limits stay.
- `system/public/ours/object.hpp` — the enum added beside the generated object declarations; the `port.hpp` include dropped.
- `kernel/object/include/ours/object/handle_table.hpp` — included port.hpp only for this enum; now includes `<ours/object.hpp>`, the ABI header of the call it implements.
- `system/lib/{ioservice,loop}/test/fake_kernel.hpp` — gain the explicit `<ours/object.hpp>` they were getting transitively.

**Symptom**

Latent, organisational: `<ours/object.hpp>` depended on `<ours/port.hpp>` for one option word, an edge Zircon does not have — `zircon/syscalls/object.h` includes nothing of `syscalls/port.h`.

**Cause**

`WaitAsyncOptions` was filed with the packet types because a wait-async packet eventually queues on a port. But the option word belongs to the call that takes it, and nothing on the port side ever reads it: the kernel's port observer is handed the already-resolved value. The dependency was the file layout's, not the domain's.

**Change**

The enum moved to `object.hpp` with its comment, and the header's port include went with it. `PortPacket` stays in `port.hpp`: the packet is the port's business, the option is not. `handle_table.hpp` — whose port include existed only for this enum — now names the header of the call it serves. The test fake kernels that emulate `object_wait_async` name `<ours/object.hpp>` explicitly instead of getting the enum by accident of include order.

**Reference**

`zircon/system/public/zircon/syscalls/object.h` — carries `zx_object_wait_async` with no port include; its option word is a plain `uint32_t`, so the question does not arise there. Ours keeps the typed enum and puts it where the type's caller lives.

**Verification**

`boot_check.py`: build ok; dbg 45 passed / rel 43 passed (7/7 required), both `reached: end`; host tests 159 passed — the ioservice and loop suites compile for the host against the moved enum.

**Deliberately not done**

`PORT_MAX_OBSERVERS` stays in `port.hpp`: it bounds a resource the port owns, whatever call arms it.

**How it was found**

REVIEW.md M1 of this task, written during the refactor's self-review.

**The user's prediction**

None — a mechanical follow-up the user asked to have resolved.

**Three questions you should be able to answer**

- Why does the option word live with the call rather than with the packet its call eventually queues?
- Which kernel header changed include because it held the enum for no domain reason, and what does it include now?
- What stays in `port.hpp` after the move, and why?
