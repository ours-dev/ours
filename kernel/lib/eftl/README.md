# eftl — ELF Format Template Library

**eftl** is a modern C++ (≥20) header-only library for parsing, validating, and
interpreting ELF (Executable and Linking Format) binaries.  It models every
major ELF feature through strongly-typed, zero-overhead abstractions and is
designed for use in kernels, linkers, loader tools, and binary analysis.

## Scope

- Parse and validate arbitrary ELF files from raw memory.
- Navigate ELF structures — headers, program headers, section headers, symbol
  tables, dynamic entries, relocation tables, hash tables, versioning
  information, notes, and compression metadata.
- Load ELF segments into virtual memory with customizable policies
  (page-aligned mapping, RELRO carving, load-address selection).
- Operate on both 32-bit and 64-bit ELF through a single set of templates.
- Handle both little-endian and big-endian encodings transparently.

## Architecture

### Parser returns a ready-to-use elf::Binary

```cpp
auto result = eftl::elf::Parser::parse(data, size);
//   └─ Result<eftl::elf::Binary, ParseError>
//      eftl::elf::Binary IS-A eftl::Binary — ready to use immediately
```

`eftl::elf::Binary` hides all 32/64-bit dispatch internally.  Users never
see `FileImpl<…>`, `BasicImage<…>`, or a `Variant`.  Non-loadable
files (ET_REL, ET_CORE) fail with `ParseError::NotLoadable`.

- **[binary.hpp](include/eftl/binary.hpp)** — Abstract `Binary` type with an
  inner `Binary::FormatId` enum (`Elf`, `PE`, `MachO`).  Every binary format
  derives from this.
- **[elf/binary.hpp](include/eftl/elf/binary.hpp)** — `FileImpl<Traits>`:
  the central ELF parser template.  Constructed from raw bytes; validates
  headers, program headers, section headers, and optional extended sections
  (symbols, dynamics, relocations, hashes, versions, notes, TLS, RELRO,
  GNU properties, compressed sections).  `File` (non-template) wraps a
  variant of `FileImpl<Traits32>` / `FileImpl<Traits64>`.
- **Traits** — `Traits32` / `Traits64` bundle the type aliases and constants
  for each ELF class so that the rest of the library is written once as
  templates.  `TraitsNative` selects 32- or 64-bit at compile time.

### Key templates (all under `eftl::elf`)

| Header | Primary Type | Purpose |
|--------|-------------|---------|
| `section.hpp` | `SectionView<Traits>` | Section-header accessor |
| `segment.hpp` | `SegmentView<Traits>` | Program-header accessor |
| `table.hpp` | `TableView<Entry>` | Generic contiguous table with validated decoding |
| `symbol.hpp` | `SymbolTableView<Traits>` | Symbol table + extended section indices |
| `dynamic.hpp` | `DynTableView<Traits>` | Dynamic entry (`DT_*`) table with tag-based cached views |
| `dynamic_section.hpp` | `DynamicSectionView<Traits>` | `SHT_DYNAMIC` wrapper |
| `dynamic_image.hpp` | `DynamicImageView<Traits>` | High-level query interface over a dynamic segment |
| `dynamic_linkage.hpp` | `DynamicLinkageView<Traits>` | Symbol lookup, PLT/GOT relocation, initialisation ordering |
| `dynamic_array.hpp` | `DynamicFunctionArrayView<Traits>` | `DT_INIT_ARRAY` / `DT_FINI_ARRAY` / `DT_PREINIT_ARRAY` |
| `sysv_hash.hpp` | `SysvHashTableView<Traits>` | Classic ELF hash-table lookup |
| `gnu_hash.hpp` | `GnuHashTableView<Traits>` | GNU-style hash-table lookup (bloom filter + buckets + chains) |
| `relocations.hpp` | `Relocations<Traits>` / `RelrTableView<Traits>` | REL, RELA, and packed RELR relocation application |
| `relocation_section.hpp` | `RelocationTableView<Rel>` | Per-section relocation table |
| `version_binding.hpp` | `VersionResolver<Traits>` | Resolves symbol-version bindings (local, global, definition, requirement) |
| `version_symbol.hpp` | `VersionSymbolTableView<Traits>` | `SHT_GNU_versym` table |
| `version_definition.hpp` | `VersionDefinitionView<Traits>` | `SHT_GNU_verdef` with auxiliary entries |
| `version_requirement.hpp` | `VersionRequirementView<Traits>` | `SHT_GNU_verneed` with auxiliary entries |
| `tls.hpp` | `TlsTemplate<Traits>` | PT_TLS template validation |
| `gnu_stack.hpp` | `GnuStack<Traits>` | PT_GNU_STACK execution flags |
| `gnu_property.hpp` | `GnuPropertyNoteView<Traits>` | `.note.gnu.property` parsing |
| `relro.hpp` | `RelroRegions<Traits>` | PT_GNU_RELRO region tracking |
| `note.hpp` | `NoteSectionView<Traits>` | Generic ELF note-section parser |
| `group.hpp` | `SectionGroupView` | SHT_GROUP (COMDAT) validation |
| `compression.hpp` | `SectionCompressionView<Traits>` | ZLIB / ZSTD compressed section metadata |
| `load.hpp` | `SegmentLoader<Traits>` | Loads segments into a provided memory region with validation |
| `load_policy.hpp` | `LoadPolicy<Traits>` | Computes load-address layout from segments, and reports the PT_GNU_RELRO ranges alongside it |
| `mapped_memory.hpp` | `MappedMemory` | Address-space abstraction for loaders |
| `link.hpp` | (free functions) | Base-address biasing and relative-relocation helpers |
| `parser.hpp` | `ElfParser<Traits>` | Entry-point: construct an `ElfFile` from raw bytes |
| `encoding.hpp` | (free functions) | Endian-aware scalar decode/encode |
| `checked_reader.hpp` | `CheckedReader` | Bounds-checked sequential read from a `BinaryStream` |
| `observable.hpp` | `Observer<Traits>` | Observer pattern for progressive segment loading |

### Support types

| Header | Contents |
|--------|----------|
| `enums.hpp` | `Arch` (60+ machine types), `SectionType`, `SectionFlags` |
| `traits.hpp` | `ElfClass`, `ElfTraits32`, `ElfTraits64`, `ElfTraitsNative` |
| `details/structures.hpp` | Raw ELF struct layouts (`Elf32Ehdr`, `Elf64Phdr`, …) |
| `details/types.hpp` | Raw ELF type aliases (`Elf32Addr`, `Elf64Word`, …) |
| `details/dyn.hpp` | Dynamic-tag flags (`DF_*`) and feature-bit helpers |
| `details/segment.hpp` | Segment-type flags (`PF_*`) |
| `views.hpp` | Re-exports of commonly used view types |
| `relocation_catalog.hpp` | Complete x86 / x86‑64 relocation metadata tables |
| `config.hpp` | Compiler-attribute forwarding |

### Facade

**[views.hpp](include/eftl/elf/views.hpp)** — `View<Traits>::from_raw()` chains
`FileImpl` → `BasicImage` → `DynamicImageView` into a single call and
exposes a unified query interface: segments, sections, dynamics, entry point,
virtual-address range, machine type, and `apply_relocation()`.

## Design

- **Zero-copy.** Every view is a borrowed span over the input buffer.  No
  heap allocation, no copying of ELF content.
- **Eager structural validation.** `ElfFile::from_raw()` validates the entire
  container up front — magic, class, endianness, extended numbering,
  alignment invariants, duplicate/overlapping entries, and cross-table
  consistency.  Errors are returned as fine-grained `ParseError` values.
- **Traits-parameterized.**  All templates accept `ElfTraits` (32- or 64-bit).
  `ElfParser::parse()` auto-dispatches by `EI_CLASS` at runtime; the rest of
  the code is monomorphised at compile time.
- **Observer-pipeline decoding.** Segment and dynamic-entry processing uses
  a CRTP observer pattern (`Observable<Traits, Derived>`) so consumers can
  hook into the decode pass without subclassing the core types.
- **Non-goal — no execution.** eftl validates, describes, and prepares ELF
  structures.  It never executes code, decompresses data, or resolves
  symbolic relocations.  Symbolic-relocation application is explicitly
  rejected by the observer pipeline; only relative relocations are applied.

### Consumer

The primary consumer is the **phys (boot) layer** at
`kernel/phys/elf-image.cpp`.  It uses `ElfParser`, `KernelElfLoadPolicy`
(targeting X86_64, I386, AARCH64, and RISCV), `MappedMemory`, and the
relocation observer pipeline to load and relocate the kernel ELF at boot
time.

## Supported ELF features

- **ELF classes**: 32-bit (`ELFCLASS32`) and 64-bit (`ELFCLASS64`)
- **Byte orders**: little-endian (`ELFDATA2LSB`) and big-endian (`ELFDATA2MSB`)
- **Machine architectures**: full `e_machine` enumeration (60+ values); x86
  and x86‑64 relocation catalogs are included inline
- **Section types**: `PROGBITS`, `SYMTAB`, `STRTAB`, `RELA`, `REL`, `HASH`,
  `DYNAMIC`, `NOTE`, `NOBITS`, `DYNSYM`, `INIT_ARRAY`, `FINI_ARRAY`,
  `PREINIT_ARRAY`, `GROUP`, `SYMTAB_SHNDX`, `GNU_HASH`, `GNU_VERDEF`,
  `GNU_VERNEED`, `GNU_VERSYM`, `GNU_ATTRIBUTES`
- **Segment types**: `LOAD`, `DYNAMIC`, `INTERP`, `NOTE`, `TLS`, `PHDR`,
  `GNU_EH_FRAME`, `GNU_STACK`, `GNU_RELRO`, `GNU_PROPERTY`
- **Dynamic tags**: full `DT_*` enumeration with tag-indexed caching;
  supports `DT_NEEDED`, `DT_SONAME`, `DT_RPATH`, `DT_RUNPATH`, `DT_INIT`,
  `DT_FINI`, `DT_INIT_ARRAY`, `DT_FINI_ARRAY`, `DT_PREINIT_ARRAY`,
  `DT_HASH`, `DT_GNU_HASH`, `DT_STRTAB`, `DT_SYMTAB`, `DT_STRSZ`,
  `DT_SYMENT`, `DT_REL`, `DT_RELA`, `DT_RELASZ`, `DT_RELAENT`, `DT_RELR`,
  `DT_RELRSZ`, `DT_RELRENT`, `DT_PLTREL`, `DT_PLTGOT`, `DT_JMPREL`,
  `DT_PLTRELSZ`, `DT_BIND_NOW`, `DT_FLAGS`, `DT_FLAGS_1`, `DT_VERDEF`,
  `DT_VERNEED`, `DT_VERSYM`, `DT_VERDEFNUM`, `DT_VERNEEDNUM`,
  `DT_SYMBOLIC`, `DT_TEXTREL`, `DT_DEBUG`, `DT_RELACOUNT`, `DT_RELCOUNT`,
  `DT_GNU_CONFLICT`, `DT_GNU_LIBLIST`, `DT_GNU_CONFLICTSZ`,
  `DT_GNU_LIBLISTSZ`, and more
- **Relocations**: `REL`, `RELA`, and packed `RELR` with complete x86 /
  x86‑64 type catalogs
- **Hash tables**: classic SYSV (Elf32_Word buckets + chains) and GNU
  (bloom filter + buckets + chains)
- **Symbol versioning**: definitions (`verdef`), requirements (`verneed`),
  symbol-version table (`versym`), and resolved `VersionBinding`
- **TLS**: PT_TLS template validation
- **GNU extensions**: `PT_GNU_STACK`, `PT_GNU_RELRO`, `.note.gnu.property`,
  `SHT_GNU_HASH`
- **Section groups**: `SHT_GROUP` (COMDAT) with duplicate-member validation
- **Compressed sections**: ZLIB and ZSTD (`ELFCOMPRESS_ZLIB`,
  `ELFCOMPRESS_ZSTD`)

## Dependencies

- **[ustl](../../../system/lib/ustl/)** — `Option<T>`, `Result<T, E>`, `Span<T>`, `StaticVec<T, N>`
- **[ours/types.hpp](../../../system/public/ours/types.hpp)** — fixed-width integer aliases (`u8`, `u16`, `u32`, `u64`, `usize`)

## Usage example

```cpp
#include <eftl/elf/parser.hpp>

auto const result = eftl::elf::Parser::parse(data, size);
if (result.is_err()) {
    // handle ParseError (includes NotLoadable for ET_REL / ET_CORE)
}
auto const &binary = result.value();   // eftl::elf::Binary, IS-A eftl::Binary

// Use directly — no native_image() step needed
auto const entry  = binary.entrypoint();              // u64
auto const base   = binary.imagebase();               // u64
auto const offset = binary.virtual_address_to_offset(entry);
auto const klass  = binary.class_type();              // Class::Elf32 or Elf64
auto const machine = binary.machine();                // Arch::X86_64, etc.

// Advanced: access the underlying typed BasicImage
binary.with_impl([](auto const &img) {
    // img is BasicImage<Traits32> or BasicImage<Traits64>
});
```

### Typed (advanced — when you know the ELF class at compile time)

```cpp
auto const result = eftl::elf::Parser::parse_as<eftl::elf::Traits64>(data, size);
if (result.is_ok()) {
    auto const &file = result.value();  // eftl::elf::FileImpl<Traits64>
    auto const segments = file.segments();
    auto const sections = file.sections();
    // Full typed API available — no variant overhead
}
```

## Coding conventions

- Modern C++ (≥20), header-only.
- `const`/`constexpr`-first; mutable only where necessary.
- Trailing return types on all functions.
- `[[nodiscard]]` on every value-returning function.
- `enum class` for all enumerations.
- `using X = Y;` (no `typedef`).
- `nullptr` (no `NULL` or `0` as pointer).
- Braced control-flow bodies, even for single statements.
- No ternary operators — always explicit `if`-`else`.
- No macros beyond include guards.
- Per-library `.clang-format` / `.clang-tidy` configuration.

## Testing

33 test suites cover every public header.  Build and run with the project's
GN-based build system:

```
gn gen out && ninja -C out kernel/lib/eftl:tests
```
