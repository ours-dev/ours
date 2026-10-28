# riscv-phys outstanding repairs

## 1. physload runs physboot's code on riscv without a `fence.i` after copying it

**Severity**
Medium: on riscv hardware with an instruction cache, physboot's constructors and entry may execute stale bytes; not reached today, since riscv phys has only run under QEMU, whose TCG never fetches stale instructions.

**Symptom**
Latent, cannot be triggered today. It would show on real riscv hardware as physboot crashing or misbehaving at its first instructions, nondeterministically, depending on what the instruction cache held for the pages bootmem gave it. `llvm-objdump -d` finds no `fence.i` in `physload`.

**Cause**
`ModuleLoader::load()` (`kernel/phys/module_loader.cpp`) copies physboot into bootmem with `ElfImage::load_at()` and patches it with `relocate_and_bind()`, all ordinary stores. `ModuleLoader::start()` then calls `run_initializers()` and `m_descriptor->init`, which execute that code. RISC-V does not keep a hart's instruction fetches coherent with its own stores; only `fence.i` (Zifencei) makes them visible to later fetches on the same hart. Nothing between the stores and the first call issues one. The weak `arch_on_phys_load_handoff()` cannot carry it, because it runs after `run_initializers()` has already executed module code. x86 is unaffected: its instruction fetches snoop stores. physboot's own jump to the kernel already has its fence (`kernel/arch/riscv/phys/handoff.cpp`), because the kernel image it loads is in this lane's files and the loader is not.

**Change**
End `ElfImage::load_at()` with an architecture hook that makes the loaded range coherent for instruction fetch: `fence.i` on riscv, nothing on x86, and on arm64 a data-cache clean and instruction-cache invalidate over the range. Then every image phys loads, physboot and the kernel alike, is safe to jump into when `load_at()` returns, and the `fence.i` in riscv's `arch_handoff_to_kernel()` becomes redundant (keep it or drop it in the same change, and say which). Ruled out: a `fence.i` in `ModuleLoader::start()` only, since it would leave every other `load_at()` caller to remember the same thing.

**Reference**
Zircon `ElfImage::Load()` (`kernel/phys/elf-image.cc`) ends with `arch::GlobalCacheConsistencyContext` and `SyncRange()` over the loaded image; its riscv destructor is `fence.i` (`kernel/lib/arch/riscv64/include/lib/arch/cache.h`).

**Acceptance**
`llvm-objdump -d` of riscv `physload` shows a `fence.i` reached from `ElfImage::load_at()` before it returns; x86 `ours-verify` still passes; the riscv integration boot reaches physboot's first log line.

## 2. `Aspace`'s members have no initialisers, so an architecture that skips `set_allocation_bounds()` reads indeterminate values

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
