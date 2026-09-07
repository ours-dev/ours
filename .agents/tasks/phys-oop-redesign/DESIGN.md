# phys-oop-redesign

## Problem

`kernel/phys` is already half object-oriented — `multiboot::View`, `BootObi`,
`KernelPackage`, `BootPackageInput`, `ElfImage`, `ModuleLoader`, `Aspace`,
`KernelPackageLoader` are all validated-view / loader classes — but several
clusters are still free functions, and a few of them leave their state on the
caller:

1. **`boot_options.cpp`**: `apply_boot_options(BootOptions&, CommandLineView)`
   operates on a plain struct from outside.
2. **`memory.cpp`**: the whole stage-3 chain (`init_memory`,
   `obi_init_memory`, `arch_detect_memory`'s default body, and the shared
   `apply_memory_range`) is four free functions passing `bool*` out-params.
3. **`aspace.cpp`**: `create_identity_map_for_ram(Aspace&, limit)` is a free
   function whose entire subject is the `Aspace` it takes.
4. **`lib/bootshim/legacy_memory.hpp`**: `convert_memory_map`/`find_scratch`/
   `to_obi_memory_type` are free functions; the caller stacks the ranges array
   by hand and threads the conversion result through two call sites
   (shim.cpp).
5. **`elf_image.cpp` / `elf_image_mapping.cpp`**: anonymous-namespace helpers
   (`to_usize` — duplicated in both TUs, `map_permissions`, `page_count`,
   `clip_relro` + `PageRun`, `bind_rela_table` — one free function template
   with six parameters, called twice); `fix_mapping` is a ~90 line loop whose
   working state (run bounds, per-run permissions) is locals.
6. **Scattered**: `join_path` (kernel_package_loader.cpp), `own_build_id`
   (module_loader.cpp), `alloc_top_down` (physboot.cpp) + `bootmem_allocate`
   (module_loader.cpp) — two spellings of the same top-down-or-panic
   allocation policy in two *different ELF images*.

**Admission criterion** (made explicit after expert review): a cluster gets an
object where state exists or a decomposition is *re-called in a loop*; a
single-caller straight-line step stays a function. This is the same line that
keeps physboot.cpp's flow helpers out.

## Design (revised after 4-expert review — boot-chain, C++ API, linker/ABI, YAGNI)

Style constraints taken from the surrounding code: validated types are
constructed via `static auto from(...) -> ustl::Result<T, Status>` with private
constructors; errors are `ours::Status`; no allocation, no panics below the
orchestration layer unless the call site already panicked; headers carry the
why-comments — every move below preserves the existing comments by carrying
them to the new home.

### 1. `BootOptions` becomes a class

`include/ours/phys/boot_options.hpp`:

- Data members become `private` (same three fields, **same order** — the
  layout crosses `PhysLoadHandoff::boot_options` into loaded modules; access
  control is not part of layout, and `BootOptions{}` value-init stays legal).
- Public inline accessors: `aslr_disabled() -> bool`,
  `next_module() -> std::string_view`, `kernel_name() -> std::string_view`.
  **Inline on purpose**: the accessor body compiles into the caller's image,
  so cross-image readers (physboot reads through the handoff pointer) need no
  export-table entry at all — and even weak COMDAT emissions stay out of
  `.phys.exports` because gen-phys-exports.py only collects `Global` binding.
  The PT_INTERP build-id pairing (module_loader.cpp) independently forces
  same-build binaries, which covers the rest.
- `auto apply(obi::CommandLineView const &cmdline) -> Status` replaces the
  free `apply_boot_options`, keeping PHYS_EXPORT and the layering semantics
  (only overwrites options the command line actually names — physload applies
  the baked-in config first, then the bootloader cmdline on top).
- `parse_bool` / `parse_name` **stay in the .cpp's anonymous namespace**
  (review: privatizing them would widen the public header with raw
  `char*`/`usize` signatures for helpers with exactly one caller, which the
  anonymous namespace already encapsulates).
- Deleted: the free function. Call sites: `physload.cpp` ×2 becomes
  `BOOT_OPTIONS.apply(...)`, `module_loader.cpp` reads `options.next_module()`,
  `physboot.cpp` reads the two accessors, and
  `kernel/lib/obi/tests/boot_options_smoke.cpp` is rewritten to the method
  form (~10 mechanical field-access rewrites; the apply-stacking assertions
  already present are preserved as-is).
- `extern BootOptions BOOT_OPTIONS` + `global_boot_options()` unchanged.
- **In passing**: fix module_loader.cpp:119's latent log bug (two `{}` but one
  argument) while rewriting that line to the accessor.

### 2. `MemoryProvisioner` owns the stage-3 chain (stateful: injected `BootMem&`)

New `include/ours/phys/memory_provisioner.hpp` + the `memory.cpp` body.
Review verdict: a bare namespace of four functions would violate the design's
own "objects where state exists" rule, so the class holds the one thing the
old chain read implicitly — the `BootMem` it drives. That also makes the
whole policy host-testable (bootmem is header-only; `physical_memory_smoke`
already constructs a fixture on the host):

```cpp
class MemoryProvisioner final {
  public:
    /// Not PHYS_EXPORT: no module-side caller (modules bind against
    /// physload's exports, and memory.cpp only links into physload.core).
    explicit MemoryProvisioner(BootMem &memory);

    /// Stage 3a — memory discovery.  The OBI `memory` item when the
    /// container carries one, otherwise the legacy ranges through the
    /// `arch_detect_memory` hook.  Panics on an invalid config.
    auto detect(obi::ContainerView const &package,
                std::span<obi::MemoryRange const> legacy_ranges) -> void;

    /// Stage 3b — reserve [PHYS_LOAD_START, PHYS_LOAD_END).
    auto reserve_boot_image() -> void;

    /// Stage 3c — reserve the outer OBI package.
    auto reserve_package(BootPackageInput const &input) -> void;

    /// Stage 3d — bind the aspace, run the arch hook, page-align the arena.
    auto install(Aspace *aspace) -> void;

  private:
    BootMem &m_memory;
};
```

**Fallback semantics (pinned — this is the line that must not drift)**:
the legacy fallback triggers only when the container carries **no `memory`
OBI item at all** (`memory.unwrap().is_none()` ⇒ `ok(false)` ⇒ fallback).
An item that *is present but contains no Ram range* is an error path:
`InvArgs` ⇒ panic. (The original design text said "no Ram range at all",
which would have turned a panic into a silent fallback.)

- Range registration/validation moves to a **TU-local free function**
  `register_range(BootMem&, obi::MemoryRange const&, bool *added_usable)`
  (the old `apply_memory_range` verbatim, with the memory parameter made
  explicit). It stays free because the WEAK `arch_detect_memory` default
  body — also in memory.cpp — needs it, and a private member would be
  unreachable from there.
- `arch_detect_memory` keeps its `WEAK_LINK` default body (free function);
  **`arch_init_memory` is a strong symbol, not a WEAK hook** — no default
  body exists, only x86 defines it, and it must stay that way (adding WEAK
  would turn a missing arch implementation into a null-call instead of a
  link error). Review note: do not "unify" the two hooks.
- `install()` preserves the old asymmetry verbatim: `if (aspace != nullptr)
  ASPACE = aspace;` followed by an *unconditional* `arch_init_memory(aspace)`
  (x86's implementation handles null itself), then `trim_usable(PAGE_SIZE)`.
  Ordering pinned: ASPACE assignment precedes the arch hook.
- `detect()`'s panic/log wordings are carried verbatim: "Invalid OBI memory
  configuration: {}", "Legacy platform memory range could not be
  registered: {}".
- `init_memory` and its `init.hpp` declaration are deleted; `obi_main`
  drives the four steps. Comments referencing the old names
  (`kernel/mm/init_pmm.cpp`, `boot_package_input.hpp`, `init.hpp`) are
  updated in the same commit.

### 3. The RAM identity map becomes an `Aspace` member

`create_identity_map_for_ram(aspace, limit)` →
`aspace.map_usable_ram(PhysAddr max_limit)` (name mirrors bootmem's
"Usable"/`MemoryView::Usable` vocabulary). The dead default argument is
dropped — the only caller (`kernel/arch/x86/phys/aspace.cpp`) always passes
`ARCH_PHYSMAP_SIZE`. Note: as a member of the class-level-PHYS_EXPORT
`Aspace`, it joins `.phys.exports` exactly like `map`/`unmap` already do;
accepted as consistent with the class's existing posture.

### 4. `bootshim::LegacyMemoryTranslator` (all-inline)

`lib/bootshim/legacy_memory.hpp` — the file's why-comments (truncation
reporting, floor-only protection, alignment power-of-two rule) all survive;
the ranges array comes home. **Every member is inline**: lib/bootshim is a
deliberate header-only seam (host toolchain cannot evaluate //kernel/phys
targets), so an out-of-line member would break it.

```cpp
class LegacyMemoryTranslator final {
  public:
    struct Conversion final { usize converted; usize dropped; };

    /// Rewrite `map` into obi::MemoryRange vocabulary, in map order, up to
    /// MAX_LEGACY_MEMORY_RANGES; the tail is counted, not written.
    /// A second convert() on the same object resets it (pinned; smoke-tested).
    [[nodiscard]] auto convert(MemoryMap const &map) -> Conversion;

    /// The converted prefix — size() always equals Conversion::converted.
    [[nodiscard]] auto ranges() const -> std::span<obi::MemoryRange const>;

    /// Bump-allocator placement over the platform's own map (stateless).
    [[nodiscard]]
    static auto find_scratch(MemoryMap const &map, PhysAddr floor, usize size,
                             usize alignment) -> ustl::Option<PhysAddr>;

    /// Everything that is not plain Available folds to Reserved.
    [[nodiscard]]
    static constexpr auto to_obi_type(u32 multiboot_type) -> obi::MemoryRangeType;

  private:
    LegacyMemoryRanges ranges_{};
    usize converted_{0};
};
```

- One reporting channel: the `Conversion` return (shim's truncation warning
  reads its numbers from it); `converted()`/`dropped()` accessors are *not*
  added. `ranges()` exposes only the converted prefix, making
  `ranges().size() == converted()` an invariant the smoke asserts, together
  with the reset-on-reconvert semantics.
- `MAX_LEGACY_MEMORY_RANGES` / `LegacyMemoryRanges` stay namespace-level
  (shared vocabulary with `LegacyPlatformInfo` consumers).
- shim.cpp stacks the translator — the "stack-local, not scratch-allocated"
  lifetime comment now points at the object.
- `legacy_memory_smoke.cpp` retargets to the class; existing boundary cases
  (0 / MAX / MAX+7) convert to method-call form.

### 5. The ELF loader: one named pass object, TU placement preserved

The elf_image_mapping.cpp file split exists for a link reason (boot-shim
links elf-image.cpp without Aspace/BootMem) — both files keep their TU
placement; only anonymous functions become named types. Review verdict
adopted: **`RelativeRelocationPass` is dropped** — `apply_kernel_relative_
relocations` has one call site, construct-once-call-once, the exact
"ceremony over a procedure" the design's own exclusion list names. It, its
`dynamic_entries` helper, and both relocation-type predicates stay TU-local
free functions in elf_image.cpp.

- **New private header `kernel/phys/detail/numeric.hpp`**
  (`namespace ours::phys::detail`): `checked_to_usize<Value>(Value) ->
  ustl::Option<usize>` replacing the two duplicated `to_usize` copies in
  phys, plus `page_count(byte_count)`. Host-checked: ustl ships no
  checked-cast utility. Scope note: eftl has its own identical helpers
  (version_detail.hpp et al.) and *keeps them* — eftl sits below phys and
  must not include `ours/phys/detail/`; the claim is "phys-local duplication
  eliminated", not "duplication eliminated".
- **`elf_image.cpp` — TU-local `SymbolicRelocationPass`** (kept: two call
  sites, six threaded parameters each — real deduplication):

  ```cpp
  class SymbolicRelocationPass final {
    public:
      SymbolicRelocationPass(MappedMemory<Traits> memory,  // by value: a
                             typename Traits::Addr bias,   // lightweight view
                             eftl::elf::Arch machine,
                             DynamicSymbols const &symbols,
                             SymbolBinder const &binder);
      template <typename Relas>
      [[nodiscard]] auto bind(Relas const &relocations) -> Status;
    private:
      static auto symbolic_relocation_type(eftl::elf::Arch, u32) -> bool;
      MappedMemory<Traits> m_memory;
      typename Traits::Addr m_bias;
      eftl::elf::Arch m_machine;
      DynamicSymbols const &m_symbols;
      SymbolBinder const &m_binder;
  };
  ```

  `relocate_and_bind()` constructs the pass once and calls `bind()` twice
  (.rela.dyn and .rela.plt). The SHN_UNDEF/SHN_ABS/bias block comment moves
  into `bind()`. `machine` lives on the pass, so the predicate can be a
  private static (the review caught that the original sketch had the
  predicate private but the machine outside — incoherent).
- **`elf_image_mapping.cpp` — `SegmentMapper`** (kept: its decompositions
  `map_segment`/`map_run` are called in the walk loop and carry running
  arithmetic state — a loop-called stateful decomposition is the admission
  criterion's target case):

  ```cpp
  class SegmentMapper final {
    public:
      SegmentMapper(Aspace &aspace, ElfImage::LoadPlan const &plan,
                    VirtAddr target_base, PhysAddr pa_load);
      auto map_all() -> Status;   // relro offset validation + segment walk
    private:
      struct PageRun { VirtAddr first; VirtAddr last; };
      static auto clip_relro(...) noexcept -> PageRun;  // comment kept verbatim
      static auto map_permissions(SegPerms) noexcept -> MmuFlags;
      auto map_segment(/* segment */) -> Status;        // the run-walking loop
      auto map_run(VirtAddr first, VirtAddr last, PhysAddr pa, MmuFlags) -> Status;
      Aspace &m_aspace;
      ElfImage::LoadPlan const &m_plan;
      VirtAddr m_target_base;
      PhysAddr m_pa_load;
  };
  ```

  **Access**: `ElfImage::LoadPlan` is a private alias, so `ElfImage` declares
  `class SegmentMapper; friend class SegmentMapper;` and `SegmentMapper` is
  defined at *namespace scope* in elf_image_mapping.cpp (a friend declaration
  never matches an anonymous-namespace entity). "TU-local" therefore relaxes
  to "defined only in this TU" — the boot-shim link story is untouched (it
  never links this TU and never calls fix_mapping). No `image_size` member:
  the plan already reports it.
  The "relocate before fix_mapping, through the identity-mapped physical
  pages" **caller-order contract moves up to the `fix_mapping` declaration**
  in elf_image.hpp (repo rule: headers carry why-comments); the walk
  mechanics comments stay on map_all/map_segment.

### 6. Scattered helpers (narrowed by review)

- **`kernel/phys/detail/boot_alloc.hpp`** — **header-only, all inline**:
  `detail::allocate_top_down(usize size, usize alignment, std::string_view
  what) -> bootmem::Range` — the top-down-or-panic policy with `what` in the
  panic. Inline is *mandatory*, not stylistic: the two call sites link into
  two different ELF images (module_loader.cpp → physload.core;
  physboot.cpp → the physboot module, which binds against physload's export
  table and links none of its objects); an out-of-line definition would hand
  physboot a hidden undefined symbol and fail the module link. Replaces
  `bootmem_allocate` + `alloc_top_down`.
- `Aspace::alloc_page_table` deliberately *not* folded in: its contract is
  different (returns 0 instead of panicking, honors `m_allocation_*_bound`).

### Deliberately not converted

- physboot.cpp's flow helpers (`gen_load_bias`, `load_kernel_image`,
  `build_kernel_aspace`, `snapshot_memory_map`) and boot-shim's
  `load_physload`: single-caller straight-line boot choreography; the
  admission criterion keeps them functions. Revisit only if a second phys
  module needs to reuse them.
- `join_path` (kernel_package_loader.cpp), `own_build_id`
  (module_loader.cpp), `parse_string`/`parse_acpi` (legacy_multiboot.cpp),
  `parse_bool`/`parse_name` (boot_options.cpp): stateless, single-caller —
  member-izing them is decoration; anonymous namespaces already encapsulate.
- `hex_encode_build_id`: stays free — it exists *to be* phys-independent so
  it can run on the host (build_id_note_smoke.cpp).
- `make_acpi_table_set` (acpi.cpp): one-line factory; the TU-local
  `IdentityMapper` is already the polymorphic object the design wants.
- eftl's `to_usize` copies: below phys in the layer stack (see §5).

## 顺手修正 (behavior deviations, recorded)

1. Panic-message unification in the shared allocator: module_loader's
   "No enough memory to place phys module" (also ungrammatical) becomes
   `allocate_top_down`'s "No memory to place {what}" — one string per call
   site, passed by the caller.
2. phys-local `to_usize` duplication removed (behavior-identical; eftl keeps
   its own copies for layering reasons).
3. module_loader.cpp:119's log call gains its missing second argument
   (`DEFAULT_MODULE`) — pre-existing bug on a line the refactor rewrites
   anyway.
4. No parsing/validation/error-code changes anywhere else; log lines keep
   their shape (shim's truncation warning reads its numbers from the
   translator's Conversion). Diff audit item: the only panic/log wording
   change in the whole series must be #1.

## Validation

- **Host smokes** (explicit targets — the existing out/mm-host-tests graph
  does not contain boot-options-smoke / build-id-note-smoke / boot-obi-test;
  they need their own `gn gen`): `gn gen` a host-tests directory and ninja
  `boot-options-smoke`, `legacy-memory-smoke`, `multiboot` view/memory-map
  smokes, `physical-memory-smoke`, `build-id-note-smoke` (exact target
  spellings from kernel/lib/obi/BUILD.gn, kernel/phys/BUILD.gn,
  kernel/phys/lib/*/BUILD.gn at execution time). All green after every
  commit.
- **End-to-end**: `scripts/qemu-boot.sh` (OUT=out/bringup.x86) after **every**
  commit, pass criterion = the "handoff: jumping to kernel" log line.
  Review-driven change from "only #3 and #6": shim.cpp has zero host
  coverage (#5), and #2's cross-image inline accessors are only exercised by
  a real boot; each run is minutes, not a suite.
- Full `ninja -C out/bringup.x86` — the boot-shim link is the canary for the
  elf TU split and the header-only detail/boot_alloc.hpp constraint.

## Follow-up candidates (not this series)

- Host smoke for `MemoryProvisioner` (feasible with injected `BootMem&` —
  construct an obi container with/without a memory item in the fixture, the
  way boot_obi_test does). Deferred to keep the series reviewable; the
  injected-reference design is what makes it possible later.
- A shared home for `checked_to_usize` below phys (so eftl's copies can
  merge too) if eftl ever wants it.
