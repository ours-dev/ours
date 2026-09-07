/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ELF_IMAGE_HPP
#define OURS_PHYS_ELF_IMAGE_HPP 1

#include <ours/compiler.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/collections/static_vec.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

#include <span>
#include <string_view>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/link.hpp>
#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/relocations.hpp>

namespace ours::phys {
/// Resolves an undefined symbol of a loaded image to a definition the
/// loader already has.  A plain context pointer and function pointer rather
/// than a closure: unlike the rest of the loader/module handoff (which
/// shares concrete types freely -- physload and its modules are always
/// compiled together), this specific boundary is dynamic ELF symbol
/// resolution, where the loader has no compile-time visibility into what
/// it is binding at all.
struct SymbolBinder {
    void *context;
    auto (*resolve)(void *context, std::string_view name) -> void const *;
};

// ElfImage is not PHYS_EXPORT as a whole: relocate_and_bind() and section()
// below are ModuleLoader's own tools for loading *other* images (physboot
// among them) and have no legitimate module-side caller.  Class-wide export
// would hand every module a way to load and relocate arbitrary images of its
// own -- exactly the kind of accidental surface widening PHYS_EXPORT's
// per-declaration marking is meant to prevent.  Only the four methods a
// module actually calls (to load its own kernel payload) are marked.
class ElfImage final {
  public:
    // The kernel image page-separates every permission boundary (kernel.ld),
    // which currently yields 9 PT_LOADs; keep real headroom.
    static constexpr auto MAX_LOAD_SEGMENTS = usize{12};

    /// Parse `file` and plan its load.  An ELF image describes its own
    /// in-memory size (the load plan's image_size()), so there is no
    /// caller-supplied size to validate against: size() below reports what
    /// the plan says, and load_at() needs exactly that much room.
    PHYS_EXPORT [[nodiscard]]
    auto init(std::span<u8 const> file) -> Status;
    PHYS_EXPORT [[nodiscard]]
    auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status>;
    /// Map the image's load segments at `target_base` in `aspace`, carving
    /// PT_GNU_RELRO out of its segments at map time rather than
    /// re-protecting afterwards.  Must run after relocate(): the
    /// relocations are applied through the identity mapping of the image's
    /// physical pages, never through the mapping built here (physboot
    /// relocates, then maps).
    PHYS_EXPORT
    auto fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status;

    [[nodiscard]]
    auto size() const -> usize {
        return m_load_plan.image_size();
    }

    PHYS_EXPORT [[nodiscard]]
    auto relocate(usize load_address) -> Status;

    /// Relocate, then bind the image's undefined symbols.
    ///
    /// This is the physload_module() path: the module carries no copy of the
    /// phys runtime, so every symbol it uses from physload is left undefined
    /// at link time and reaches the image as an R_X86_64_GLOB_DAT (or the
    /// equivalent) against its GOT.  `binder` maps a linker name to the
    /// address physload defines it at -- see ours/phys/exports.hpp.
    ///
    /// A name the binder does not know is an unresolved module dependency
    /// and fails the load; it is never bound to zero.
    ///
    /// ModuleLoader-only: not PHYS_EXPORT.  A module loads its own kernel
    /// payload with plain relocate(), never this.
    [[nodiscard]]
    auto relocate_and_bind(usize load_address, SymbolBinder const &binder) -> Status;

    /// A named section of the loaded image, at the address it now occupies.
    ///
    /// This is how physload finds what a module declared about itself --
    /// the PHYS_MODULE() descriptor and the constructor array -- without
    /// either side agreeing on a symbol name.  Reads the section table, so
    /// it needs an image whose section headers survived packaging; a
    /// module keeps them for exactly this reason.
    ///
    /// Only meaningful after relocate_and_bind(): pointers inside the
    /// section are fixed up by the same pass that binds everything else.
    ///
    /// ModuleLoader-only: not PHYS_EXPORT, for the same reason as
    /// relocate_and_bind() above.
    [[nodiscard]]
    auto section(std::string_view name) const -> std::span<u8 const>;

  private:
    using Traits = eftl::elf::TraitsNative;

    /// The segment walker behind fix_mapping(), defined only in
    /// elf-image-mapping.cpp (the TU whose Aspace-using code boot-shim's
    /// build must never pull in).  A friend so LoadPlan can stay private;
    /// namespace scope there -- a friend declaration never matches an
    /// anonymous-namespace entity.
    friend class SegmentMapper;

#if defined(__x86_64__)
    static constexpr eftl::elf::Arch TARGET_ELF_MACHINE = eftl::elf::Arch::X86_64;
#elif defined(__i386__)
    static constexpr eftl::elf::Arch TARGET_ELF_MACHINE = eftl::elf::Arch::I386;
#elif defined(__aarch64__) || defined(__arm64__)
    static constexpr eftl::elf::Arch TARGET_ELF_MACHINE = eftl::elf::Arch::AARCH64;
#elif defined(__riscv)
    static constexpr eftl::elf::Arch TARGET_ELF_MACHINE = eftl::elf::Arch::RISCV;
#else
#error "The phys ELF loader has no machine mapping for this compilation target"
#endif

    template <typename T>
    using LoadPlanCollection = ustl::collections::StaticVec<T, MAX_LOAD_SEGMENTS>;
    using LoadPlan = eftl::elf::LoadPlan<Traits, LoadPlanCollection>;
    using LoadPolicy =
        eftl::elf::KernelElfLoadPolicy<Traits, LoadPlanCollection, TARGET_ELF_MACHINE, PAGE_SIZE>;

    VirtAddr m_va_load{};
    PhysAddr m_pa_load{};
    /// Set when the image is an ET_DYN shared object (a loadable module):
    /// its PLT/symbolic relocation tables are bound by the caller's
    /// symbol-binding pass rather than rejected by the relative pass.
    bool m_allow_symbolic_tables{false};
    ustl::Option<eftl::elf::FileImpl<Traits>> m_binary;
    LoadPlan m_load_plan;
};
} // namespace ours::phys

#endif // OURS_PHYS_ELF_IMAGE_HPP
