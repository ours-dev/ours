/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_IMAGE_HPP
#define OURS_PHYS_KERNEL_IMAGE_HPP 1

#include <obi/container-view.hpp>
#include <ours/compiler.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <ustl/collections/static-vec.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/variant.hpp>

#include <limits>
#include <utility>
#include <span>
#include <string_view>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/link.hpp>
#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/relocations.hpp>

namespace ours::phys {
class PHYS_EXPORT RawImage final {
  public:
    [[nodiscard]]
    auto init(std::span<u8 const> file, usize memory_size) -> Status;
    [[nodiscard]]
    auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status>;
    auto fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status;
    auto relocate(usize) -> void {}

    [[nodiscard]]
    auto size() const -> usize {
        return m_memory_size;
    }

  private:
    std::span<u8 const> m_file;
    usize m_memory_size{0};
    PhysAddr m_pa_load{0};
};

/// Resolves an undefined symbol of a loaded image to a definition the
/// loader already has.  A plain context pointer and function pointer rather
/// than a closure: the same shape crosses the loader/module boundary as
/// EarlyMem does, and neither side may assume the other's C++ layout.
struct SymbolBinder {
    void *context;
    auto (*resolve)(void *context, std::string_view name) -> void const *;
};

class PHYS_EXPORT ElfImage final {
  public:
    static constexpr auto K_MAX_LOAD_SEGMENTS = usize{6};

    [[nodiscard]]
    auto init(std::span<u8 const> file, usize memory_size) -> Status;
    [[nodiscard]]
    auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status>;
    auto fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status;

    [[nodiscard]]
    auto size() const -> usize {
        return m_load_plan.image_size();
    }

    auto relocate(usize load_address) -> void;

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
    [[nodiscard]]
    auto section(std::string_view name) const -> std::span<u8 const>;

  private:
    using Traits = eftl::elf::TraitsNative;

#if defined(__x86_64__)
    static constexpr eftl::elf::Arch K_TARGET_ELF_MACHINE = eftl::elf::Arch::X86_64;
#elif defined(__i386__)
    static constexpr eftl::elf::Arch K_TARGET_ELF_MACHINE = eftl::elf::Arch::I386;
#elif defined(__aarch64__) || defined(__arm64__)
    static constexpr eftl::elf::Arch K_TARGET_ELF_MACHINE = eftl::elf::Arch::AARCH64;
#elif defined(__riscv)
    static constexpr eftl::elf::Arch K_TARGET_ELF_MACHINE = eftl::elf::Arch::RISCV;
#else
#error "The phys ELF loader has no machine mapping for this compilation target"
#endif

    template <typename T>
    using LoadPlanCollection = ustl::collections::StaticVec<T, K_MAX_LOAD_SEGMENTS>;
    using LoadPlan = eftl::elf::LoadPlan<Traits, LoadPlanCollection>;
    using LoadPolicy =
        eftl::elf::KernelElfLoadPolicy<Traits, LoadPlanCollection, K_TARGET_ELF_MACHINE, PAGE_SIZE>;

    VirtAddr m_va_load{};
    PhysAddr m_pa_load{};
    ustl::Option<eftl::elf::FileImpl<Traits>> m_binary;
    LoadPlan m_load_plan;
};

class PHYS_EXPORT KernelImage final {
  private:
    using ImageStorage = ustl::Variant<RawImage, ElfImage>;

    template <typename Function>
    FORCE_INLINE constexpr auto visit_image(Function &&function) -> decltype(auto) {
        return ustl::visit(std::forward<Function>(function), m_storage);
    }

    template <typename Function>
    FORCE_INLINE constexpr auto visit_image(Function &&function) const -> decltype(auto) {
        return ustl::visit(std::forward<Function>(function), m_storage);
    }

  public:
    [[nodiscard]]
    auto init(obi::ItemView const &item) -> Status {
        if (item.type() != obi::ItemType::Executable) {
            m_initialized = false;
            return Status::BadType;
        }
        if (item.compression() != obi::Compression::None) {
            m_initialized = false;
            return Status::NotSupported;
        }
        switch (item.format()) {
            case obi::ItemFormat::Raw:
                m_storage.emplace<RawImage>();
                break;
            case obi::ItemFormat::Elf:
                m_storage.emplace<ElfImage>();
                break;
            default:
                m_initialized = false;
                return Status::BadType;
        }

        auto const declared_memory_size = item.memory_size();
        if constexpr (sizeof(usize) < sizeof(declared_memory_size)) {
            if (declared_memory_size >
                static_cast<decltype(declared_memory_size)>(std::numeric_limits<usize>::max())) {
                m_initialized = false;
                return Status::InvalidArgument;
            }
        }
        auto const memory_size = static_cast<usize>(declared_memory_size);
        auto const status =
            visit_image([&](auto &image) { return image.init(item.payload(), memory_size); });
        m_initialized = status == Status::Ok;
        return status;
    }

    /// Initialize from a raw file image (the flat kernel-package model:
    /// the kernel ELF is a file inside the StorageKernel BootFS, with no
    /// separate memory-size declaration).  Detects ELF vs raw by magic.
    [[nodiscard]]
    auto init(std::span<u8 const> file) -> Status {
        if (file.data() == nullptr && file.size() != 0) {
            m_initialized = false;
            return Status::InvalidArgument;
        }
        auto const is_elf = file.size() >= 4 && static_cast<u8>(file[0]) == 0x7F &&
                            file[1] == static_cast<u8>('E') && file[2] == static_cast<u8>('L') &&
                            file[3] == static_cast<u8>('F');
        if (is_elf) {
            m_storage.emplace<ElfImage>();
            auto const status = visit_image([&](auto &image) { return image.init(file, 0); });
            m_initialized = status == Status::Ok;
            return status;
        }
        m_storage.emplace<RawImage>();
        auto const status = visit_image([&](auto &image) { return image.init(file, file.size()); });
        m_initialized = status == Status::Ok;
        return status;
    }

    FORCE_INLINE constexpr auto relocate(usize offset) -> void {
        if (!m_initialized) {
            return;
        }
        visit_image([offset](auto &image) { image.relocate(offset); });
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto size() const -> usize {
        return m_initialized ? visit_image([](auto const &image) { return image.size(); }) : 0;
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> {
        if (!m_initialized) {
            return ustl::err(Status::BadState);
        }
        return visit_image([phys_addr](auto &image) { return image.load_at(phys_addr); });
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status {
        if (!m_initialized) {
            return Status::BadState;
        }
        return visit_image(
            [&aspace, target_base](auto &image) { return image.fix_mapping(aspace, target_base); });
    }

  private:
    ImageStorage m_storage;
    bool m_initialized{false};
};
} // namespace ours::phys

#endif // OURS_PHYS_KERNEL_IMAGE_HPP
