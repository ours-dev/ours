/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_IMAGE_HPP
#define OURS_PHYS_KERNEL_IMAGE_HPP

#include <ours/types.hpp>
#include <ours/compiler.hpp>
#include <ours/status.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/init-fs.hpp>

#include <ustl/result.hpp>
#include <ustl/variant.hpp>
#include <ustl/views/span.hpp>

#include <eftl/elf/views.hpp>
#include <eftl/elf/relocations.hpp>

#include <obi/obi-header.hpp>

namespace ours::phys {
    class IKernelImage {
        using Self = IKernelImage;
      public:
        IKernelImage() = default;
        virtual ~IKernelImage() = default;

        virtual auto init(ustl::views::Span<u8> file) -> Status = 0;

        // virtual auto patch(ustl::views::Span<u8> patch_file) -> Status = 0;

        virtual auto size() const -> usize = 0;

        /// If successful, return the physical address of final enter point.
        virtual auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> = 0;

        virtual auto fix_mapping(Aspace &aspace) -> Status {
            return Status::Unsupported;
        }

        /// `|offset|` is just valid on KASLR enabled.
        virtual auto relocate(usize offset) -> void {}
    };

    class RawImage: public IKernelImage {
        using Base = IKernelImage;
        using Self = RawImage;
      public:
        RawImage() = default;
        virtual ~RawImage() = default;

        auto init(ustl::views::Span<u8> file) -> Status override;

        // auto patch(ustl::views::Span<u8> patch_file) -> Status override;

        auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> override;

        auto size() const -> usize override {
            return file_.size() - sizeof(header_) + header_.reserved_size;
        }

        ustl::views::Span<u8> file_;
        obi::ObiRawExeHeader header_;
    };

    class ElfImage: public IKernelImage {
        using Base = IKernelImage;
        using Self = ElfImage;
      public:
        ElfImage() = default;
        virtual ~ElfImage() = default;

        CXX11_CONSTEXPR
        static auto const kMaxLoadSegments = 6;

        /// Stub type alias for the future InitFs support.
        auto init(ustl::views::Span<u8> file) -> Status override;

        auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> override;

        auto fix_mapping(Aspace &aspace) -> Status override;

        auto size() const -> usize override {
            return loadinfo_.va_maximal_ - loadinfo_.va_minimal_;
        }

        auto relocate(usize load_address) -> void override;
      private:
        using ElfTraits = eftl::elf::ElfTraitsNative;
        using ElfView = eftl::elf::View<ElfTraits>;

        template <typename T>
        using LoadInfoCollection = ustl::collections::StaticVec<T, kMaxLoadSegments>;
        using LoadInfo = eftl::elf::LoadInfo<ElfTraits, LoadInfoCollection>;

        VirtAddr va_load_;
        PhysAddr pa_load_;
        LoadInfo loadinfo_;
        ElfView::Dynamics dynamics_;

        using Relocations = eftl::elf::Relocations<ElfTraits>;
        using InitInfo = eftl::elf::InitFiniInfo<ElfTraits>;
        using StrTableInfo = eftl::elf::StrTableInfo<ElfTraits>;
        ElfView view_;
    };

    class KernelImage {
        using Self = KernelImage;
      public:
        enum class Type {
            RawImage,
            ElfImage,
        };

        auto init(Type type, ustl::views::Span<u8> file) -> Status {
            switch (type) {
                case Type::RawImage: {
                    storage_.emplace<RawImage>();
                    break;
                }
                case Type::ElfImage: {
                    storage_.emplace<ElfImage>();
                    image_ = ustl::mem::address_of(ustl::get<ElfImage>(storage_));
                    break;
                }
            }

            DEBUG_ASSERT(image_, "Unrecognized image type");
            return image_->init(file);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto relocate(usize offset) -> void {
            image_->relocate(offset);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto size() const -> usize {
            return image_->size();
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> {
            return image_->load_at(phys_addr);
        }

        FORCE_INLINE CXX11_CONSTEXPR
        auto fix_mapping(Aspace &aspace) -> Status {
            return image_->fix_mapping(aspace);
        }

        IKernelImage *image_;

        using ImageStorage = ustl::Variant<RawImage, ElfImage>;
        ImageStorage  storage_;
    };

} // namespace ours::phys

#endif // OURS_PHYS_KERNEL_IMAGE_HPP
