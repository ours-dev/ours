# build-headers outstanding repairs

## 1. `//kernel/mm:headers` lists none of the 46 headers of `kernel/mm/include`

**Severity**
Medium: not a failure today, but 199 files include `<ours/mm/...>` (`git grep`), and `gn check` cannot hold any of those includes to a dependency while no target lists the header.

**Symptom**
Latent, cannot be triggered today. `gn desc` of `//kernel/mm:headers` shows `public = []`, and `python3 scripts/header_lists.py out/bringup.x86 --scope //kernel/mm:` reports 46 `missing` lines, one per header under `kernel/mm/include/ours/mm/` (including `priv/`). The scope has the colon because the target is `//kernel/mm:headers`; `//kernel/mm/` would match no target and fail. It would show as the first `--check-system` finding about an mm header once the list exists: today every include of one passes whether or not the includer depends on mm.

**Cause**
`cpp_interface()` forwards `public` to GN and nothing else reads it but `gn check`, so an empty list builds exactly like a full one and nothing ever asked for it to be filled (see `docs/changes/261030/kernel-arch-seam.md`, the entry on x86's `headers` target, for the same gap at 24 of 25). The target also carries `public_configs` and `public_deps` that other targets rely on, so it is not a stub.

**Change**
List the 46 headers, in groups that follow the directory (`ours/mm/` and `ours/mm/priv/`). Before editing, run `gn check --check-system --error-limit=1000000` on both build directories and count the distinct (file, line, include) pairs, as the R8 entry does, because listing will add findings for every includer without an edge; that is the check working, not a regression, and `--check-system` is not a gate. Do not add the edges in the same commit: which targets may depend on `//kernel/mm:headers` is a layering question (`ours-kernel-design`, `mm.md`).

**Reference**
None, this project's own graph.

**Acceptance**
`python3 scripts/header_lists.py out/bringup.x86 --scope //kernel/mm:` passes, the `.ninja` files do not change (the list is not an input of the build), and x86 `ours-verify` and the riscv check pass.

## 2. `//kernel/phys`'s three header targets and `//kernel/phys/lib/bootmem:headers` leave 17 headers unlisted

**Severity**
Low: the same blind spot as item 1, for a smaller root.

**Symptom**
Latent, cannot be triggered today. `--scope //kernel/phys: --scope //kernel/phys/` reports 17 `missing` (`//kernel/phys:` is the three targets of the one root, `//kernel/phys/` the bootmem library below it): 15 under `kernel/phys/include/ours/phys/` (`acpi.hpp`, `aspace.hpp`, `boot_options.hpp`, `boot_package_input.hpp`, `console.hpp`, `elf_image.hpp`, `exports.hpp`, `init.hpp`, `kernel_package_handoff.hpp`, `kernel_package_loader.hpp`, `legacy_platform_info.hpp`, `memory_provisioner.hpp`, `module.hpp`, `module_loader.hpp`, `symbolize.hpp`) and two under `kernel/phys/lib/bootmem/include/bootmem/` (`physical_memory.hpp`, `types.hpp`).

**Cause**
`:headers`, `:handoff` and `:boot-obi` publish one include root and each lists a subset, so the check judges them by the union of their lists; the 15 are in none. The three were split so that a dependant takes only the part it needs, and a header added to the root afterwards was listed by nobody.

**Change**
Decide for each of the 15 which of the three targets it belongs to (the split is by what a dependant needs, so this is a reading of each header's includers, not a mechanical sort) and list it there; list the two bootmem headers in `//kernel/phys/lib/bootmem:headers`. Count `gn check --check-system` pairs before and after on both build directories.

**Reference**
None, this project's own graph.

**Acceptance**
`python3 scripts/header_lists.py out/bringup.x86 --scope //kernel/phys: --scope //kernel/phys/` passes on both build directories; `.ninja` files unchanged; x86 `ours-verify` and the riscv check pass.

## 3. Five small kernel header targets leave 12 headers unlisted

**Severity**
Low: the same gap as item 1, one to six headers at a time.

**Symptom**
Latent, cannot be triggered today. The check reports `//kernel/lib/gktl:headers` 6 (`gktl/action.hpp`, `batch.hpp`, `canary.hpp`, `range.hpp`, `type_info.hpp`, `mixin/comparable.hpp`), `//kernel/lib/ktl:headers` 1 (`ktl/xarray.tcc`), `//kernel/lib/lockup_detector` 2 (`heartbeat_note.hpp` in `:headers`, `lockup_detector.hpp` in `:heartbeat_note`), `//kernel/lib/sampler` 2 (`session.hpp` and `stack_sample_request.hpp`, between `:core.headers`, `:headers` and `:request`) and `//kernel/platform:headers` 1 (`ours/platform/tsc.hpp`).

**Cause**
The same: the list is not an input of the build, and no check read it. `tsc.hpp` may be scaffolding for planned work (`arch-platform-seams/DESIGN.md`, I23); listing it deletes nothing and does not decide that.

**Change**
List each in the target whose role it fits; for lockup_detector and sampler, read which of the split targets the header belongs to before choosing. No behaviour changes.

**Reference**
None, this project's own graph.

**Acceptance**
`python3 scripts/header_lists.py out/bringup.x86 --scope //kernel/lib/ --scope //kernel/platform:` passes, and so does the same command on `out/bringup.riscv` (x86's directory reports 11 of the 12, riscv's all 12: `heartbeat_note.hpp` in `:headers` is a finding there only); `.ninja` files unchanged.

## 4. Five system library header targets leave 12 headers unlisted

**Severity**
Low: the same gap, in user-space libraries.

**Symptom**
Latent, cannot be triggered today. The check reports `//system/lib/ustl:headers` 4 (four under `ustl/collections/intrusive/detail/`), `//system/public:public` 3 (`ours/config/annotates.hpp`, `ours/syscall/resource.hpp`, `ours/syscall/time.hpp`), `//system/lib/ort:headers` 3 (`ort/debug.hpp`, `main.hpp`, `startup.hpp`), `//system/lib/ioservice:headers` 1 (`loop_server.hpp`) and `//system/lib/ours.resource:headers` 1 (`ours_resource/client.hpp`).

**Cause**
The same. `//system/public` is also the one root that holds files that are not headers (`BUILD.gn`, `syscalls.gni`, `public_headers.gni`); the check counts only files named like headers, so those are not findings, but a reader listing the root's headers by `ls` will see them.

**Change**
List each header in its target. `//system/public:public` takes its list from `ours_public_headers` in `public_headers.gni`, which the generated sysroot uses too, so a header added there is exported as well as listed: check that is wanted for each of the three.

**Reference**
None, this project's own graph.

**Acceptance**
`python3 scripts/header_lists.py out/bringup.x86 --scope //system/` passes; `.ninja` files unchanged.

## 5. `scripts/header_lists.py` checks only `//kernel/arch/`, not the whole tree

**Severity**
Low: a scope that was chosen for the debt above, not a defect in the check.

**Symptom**
Latent, cannot be triggered today. A header added to any root outside `//kernel/arch/` and left out of its list is not reported by `ours-verify`; only `header_lists.py --all` finds it.

**Cause**
Items 1 to 4 (87 headers in 13 include roots across the x86 and riscv build directories, no stray entries) were not fixed with the check, so it was scoped to the targets the architecture porting depends on. This item depends on items 1 to 4: widening the default first would fail every `ours-verify` run.

**Change**
When `--all` passes on both build directories, make `ALL` the default scope in `scripts/header_lists.py`, drop the "by default only the architectures' targets" paragraph from its docstring and the matching sentences in `.agents/skills/ours-verify/SKILL.md` and `boot_check.py`'s docstring.

**Reference**
None, this project's own tooling.

**Acceptance**
`python3 scripts/header_lists.py out/bringup.x86` and `... out/bringup.riscv` both pass with no `--scope` or `--all`, and `boot_check.py` prints `headers    PASS` with that wider count.

## 6. Five generic headers include a header of the per-arch set, and no check holds those includes

**Severity**
Low: nothing fails at build or run time, and `gn check` passes on both architectures; the gap is that the dependency of these headers on the architecture's headers is declared nowhere and checked by nothing.

**Symptom**
Latent, cannot be triggered today. A scan of every header outside the architectures' directories for `#include <ours/arch/NAME.hpp>`, NAME a header of a per-arch set (`aspace_layout`, `cpu_local`, `entry`, `idle_thread`, `thread`, `vm_aspace`), finds five: `heap/vmalloc.hpp` (`aspace_layout.hpp`) in `//kernel/lib/heap:headers`; `ours/mm/cpu_local.hpp` (`cpu_local.hpp`) and `ours/mm/physmap.hpp` (`aspace_layout.hpp`) in `//kernel/mm:headers`; `ours/task/thread.hpp` (`thread.hpp`) and `ours/task/idle_thread.hpp` (`idle_thread.hpp`) under `kernel/include`. `gn check --check-system` says nothing about any of them on either build directory.

**Cause**
GN holds an include only where it can resolve the file, and a `cpp_interface()` target (`configs = []`) has in its include directories only its own root, the configs it publishes and the ones its public dependencies publish, not the architecture's `kernel/include`, which the toolchain gives ordinary targets. So from a header target `<ours/arch/aspace_layout.hpp>` resolves to nothing and is ignored; a source set resolves it and is held to an edge, which is why the seven generic source sets have one (`docs/changes/261030/kernel-arch-seam.md`, the entry on them and the correction after it). Probe, on `//kernel/lib/heap:headers`: a bare `deps` edge to `//kernel/arch/$ours_cpu/kernel:headers` changes no result (x86 37 pairs, riscv 22, as before); adding `//kernel/arch/$ours_cpu/kernel:headers.config` to `public_configs` makes the finding appear (38 and 23); the config and the edge together pass (37 and 22); a `public_deps` edge instead drops x86 to 28 because every dependent receives the headers.

`//kernel/mm:headers` is also item 1: its list is empty, so its two headers are unowned until it is listed. The two under `kernel/include` have no owning target at all: `kernel/include` is the include path of the kernel products (`//kernel/products:headers`), not a `cpp_interface` root, so no list can name them.

**Change**
This is a layering decision, not a mechanical one, and belongs to the user: should a generic header target depend on the running architecture's headers, and by what. The options:
- The include-path config with an edge, on `//kernel/lib/heap:headers` (one line each: `//kernel/arch/$ours_cpu/kernel:headers.config` in `public_configs` and `//kernel/arch/$ours_cpu/kernel:headers` in `deps`, private). It is the shape `//kernel/mm:headers.config` already has in that target, and the probe shows no cycle. It forwards the architecture's include directory to every dependent of `heap:headers`, which changes the `-I` order of their command lines, so check the `.ninja` diff as the seven-target entry did. Do the same for `//kernel/mm:headers` after item 1 lists it.
- Leave them, and keep the five in this item as the known gap.
- For `ours/task/thread.hpp` and `idle_thread.hpp`: give `kernel/include` an owner (a `cpp_interface` with its root), which is a much larger change, or accept them as unowned. Do not do this one without a design.

Not chosen: a bare `deps` edge (documents a dependency and holds nothing) and `public_deps` (hides which includer needs the header).

**Reference**
None, this project's own graph.

**Acceptance**
For each header made held: removing the new edge makes `gn check out/bringup.x86 --check-system` and the riscv directory report the include (put it back), the distinct pairs are 37 and 22 with it, `gn check` passes on both, the `.ninja` diff is understood, and x86 `ours-verify` and the riscv check pass.
