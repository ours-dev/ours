/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LIB_CODE_PATCHING_CODE_PATCHING_HPP
#define LIB_CODE_PATCHING_CODE_PATCHING_HPP 1

#include <arch/types.hpp>
#include <ours/prelude.hpp>
#include <ustl/views/span.hpp>

#include <cstddef>
#include <cstdint>

namespace code_patching {

/// Opaque case identifier.  Matches the u64 emitted into `.code-patches`
/// by the `.code_patch_end` / `.code_patching.blob` assembly macros.
using CaseId = uint64_t;

/// One entry in the `.code-patches` linker section, extracted at build
/// time and handed to the kernel at boot.  Layout mirrors the three
/// `.quad` directives emitted by the assembly macros:
///
///   .pushsection .code-patches
///       .quad  <patch_site>   // address of the patchable region
///       .quad  <max_size>     // maximum bytes an alternative may occupy
///       .quad  <case_id>      // case identifier
///   .popsection
///
struct CodePatchEntry {
    /// Virtual address of the first byte that may be overwritten.
    VirtAddr patch_site;

    /// Maximum number of bytes an alternative is allowed to occupy at
    /// this site (max(default code size, largest alternative size)).
    size_t max_size;

    /// Identifies which alternative applies.  Zero means "no alternative
    /// available" — the entry is skipped at patch time.
    CaseId case_id;
};

static_assert(sizeof(CodePatchEntry) == 24,
              "CodePatchEntry must match the three-.quad layout in .code-patches");

/// A position-independent hermetic alternative blob.  The blob is a raw
/// binary produced by `extract_hermetic_blob.py` from an ELF linked with
/// `hermetic-code-blob.ld`.
struct CodePatchBlob {
    /// Pointer to the first byte of the alternative's instruction stream.
    const uint8_t *code;

    /// Size of the blob in bytes (measured from the on-disk file).
    size_t size;

    /// Which case this alternative services.
    CaseId case_id;
};

/// Architecture-specific hooks that the generic patching engine calls.
///
/// Every architecture must provide a concrete implementation — see the
/// arch code-patches headers (e.g. `<arch/code-patches/hooks.hpp>`).
struct ArchCodePatchHooks {
    /// Make the range [addr, addr + size) writable so that the patching
    /// engine can overwrite the default code.
    void (*make_writable)(VirtAddr addr, size_t size);

    /// Restore the original permissions after patching (read + execute).
    void (*make_executable)(VirtAddr addr, size_t size);

    /// Synchronise the instruction cache for the modified range.
    ///
    /// x86 needs nothing here; AArch64 needs IC IVAU + ISB.
    void (*sync_icache)(VirtAddr addr, size_t size);

    /// Given a case_id, decide whether the associated alternative should
    /// be applied on this particular machine.
    ///
    /// Returns nullptr if the alternative should NOT be applied
    /// (e.g. the CPU lacks the relevant erratum).  Otherwise returns
    /// the selected alternative blob.
    const CodePatchBlob *(*select_alternative)(CaseId case_id,
                                               ustl::views::Span<const CodePatchBlob> blobs);
};

/// Apply all code patches before any patchable code executes.
///
/// Must be called early in phys kernel boot, after memory management
/// and CPU feature detection are available but before jumping to the
/// kernel proper.
///
/// Returns the number of patches successfully applied.
[[nodiscard]]
auto apply_code_patches(ustl::views::Span<const CodePatchEntry> entries,
                        ustl::views::Span<const CodePatchBlob> blobs,
                        const ArchCodePatchHooks &hooks) -> size_t;

} // namespace code_patching

#endif // #ifndef LIB_CODE_PATCHING_CODE_PATCHING_HPP
