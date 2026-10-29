# riscv-phys outstanding repairs

## 1. `Aspace`'s members have no initialisers, so an architecture that skips `set_allocation_bounds()` reads indeterminate values

**Severity**
Low: no architecture reaches it today (x86 and riscv both call `set_allocation_bounds()` before `init()`), but one more architecture, or a reordered `arch_setup_aspace()`, would hit undefined behaviour with no compiler warning.

**Symptom**
Latent, cannot be triggered today. It would be triggered by an `arch_setup_aspace()` that calls `init()` or `map()` without first calling `set_allocation_bounds()`; riscv's first version did exactly that (`docs/changes/261028/riscv-phys.md`, the allocation-bounds section). The result depends on the stack: page-table allocations confined to a meaningless range, a null root, or the `DEBUG_ASSERT` in `Aspace::map()`.

**Cause**
`Aspace` (`kernel/phys/include/ours/phys/aspace.hpp`) declares `m_lower_pgd`, `m_upper_pgd`, `m_allocation_lower_bound` and `m_allocation_upper_bound` with no initialiser and has no constructor, and physload creates it as a plain local (`Aspace aspace;` in `obi_main()`, `kernel/phys/physload.cpp`), so all four start indeterminate. Correctness rests on every architecture knowing that `set_allocation_bounds()` is also initialisation, which nothing states.

**Change**
Give the four members `{}` initialisers. Zero bounds already mean "unbounded" to `alloc_page_table()`, and a zero root is what `init()` checks for failure, so the defaults are the values every caller wants. x86's second call, `set_allocation_bounds(0, 0)`, and riscv's only one then say a policy rather than doing initialisation; whether to keep them is the implementer's call. Ruled out: a constructor taking the bounds, since `Aspace` is built before memory discovery decides them.

**Reference**
Zircon's phys `AddressSpace` (`kernel/phys/include/phys/address-space.h`) holds the bounds as `ktl::optional<uint64_t>`, empty by default.

**Acceptance**
`aspace.hpp` declares all four members with initialisers; x86 `ours-verify` passes; riscv `physload` still builds.

## 2. physload links with `-shared`, so a missing default-visibility symbol links without an error

**Severity**
Low: a build-time diagnostic is missing; an arch layer that forgets a symbol still fails, but only at run time.

**Symptom**
Latent, cannot be triggered today: physload has no undefined symbol on x86 or riscv (`llvm-nm -u` prints nothing). While riscv's arch layer did not exist, the riscv physload link reported only `undefined hidden symbol: ours::phys::arch_init_memory(...)`; the equally missing `BOOTMEM` and `Aspace::arch_install()` were accepted silently, because they have default visibility for the module ABI. Had `arch_init_memory` also been default-visibility, physload would have linked with three unresolved references and failed when it first touched one.

**Cause**
physload is linked as a shared object (`-shared` in `config("phys-module")`, `kernel/phys/BUILD.gn`, which physload reuses so the linker emits a position-independent image with a dynamic table). A shared object may leave default-visibility symbols undefined for a dynamic linker to supply later, so lld does not report them. physload has no dynamic linker and imports nothing by design: every undefined symbol in it is a bug.

**Change**
Add `-Wl,-z,defs` to physload's own link (its `executable()` target in `phys_executable.gni`, or a config only physload uses), never to `config("phys-module")` as a whole: a `physload_module()` such as physboot imports its runtime from physload by name, so its undefined symbols are intended. Check that both passes of physload's export-table link (the `phys-prelink` pass and the final one) stay clean. Ruled out: `--no-undefined` on the shared config, which would break every module.

**Reference**
None, this project's own design: Zircon's physload is a static executable and its modules carry no symbolic imports, so the question does not arise there.

**Acceptance**
Deleting a default-visibility arch definition (for example `BOOTMEM` in an arch's `memory.cpp`) makes the physload link fail with `undefined symbol`; with it restored, x86 `ours-verify` passes and riscv `physload.bin` builds.
