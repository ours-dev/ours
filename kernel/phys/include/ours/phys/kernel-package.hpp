/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_KERNEL_PACKAGE_HPP
#define OURS_PHYS_KERNEL_PACKAGE_HPP 1

#include <obi/container-view.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <string_view>

#include <bootfs/view.hpp>

namespace ours::phys {
/// A view over the object a `kernel_package` GN template produces: a
/// `StorageKernel` item whose payload is a (possibly LZ4-compressed)
/// BootFS carrying the kernel ELF and its boot-time data files
/// (code-patches, physboot, ...).
///
/// This is the flat model matching Zircon STORAGE_KERNEL item: the
/// kernel is a *file inside the BootFS*, not a separate OBI item.
class KernelPackage final {
  public:
    using Bytes = ustl::views::Span<u8 const>;
    using MutableBytes = ustl::views::Span<u8>;
    using Result = ustl::Result<KernelPackage, ours::Status>;
    using FileResult = ustl::Result<Bytes, ours::Status>;

    /// File names inside a kernel-package directory — the C++ mirror of
    /// the kernel_package()/kernel_elf_image() GN output convention.
    /// Consumers compose "<install dir>/<name>" (e.g. "vmours/image.elf");
    /// the names themselves are fixed vocabulary, not boot parameters.
    static constexpr std::string_view kImageElf{"image.elf"};
    static constexpr std::string_view kCodePatches{"code-patches"};

    /// Parse and validate a StorageKernel item, materializing
    /// (decompressing) its payload into `output`.  `output` must be at
    /// least `storage_item.memory_size()` bytes and must outlive this view.
    [[nodiscard]]
    static auto from(obi::ItemView const &storage_item, MutableBytes output) -> Result {
        if (storage_item.type() != obi::ItemType::StorageKernel) {
            return ustl::err(ours::Status::BadType);
        }
        if (storage_item.format() != obi::ItemFormat::BootFs) {
            return ustl::err(ours::Status::BadType);
        }
        if (output.data() == nullptr && output.size() != 0) {
            return ustl::err(ours::Status::InvalidArgument);
        }
        // materialize() decompresses (Lz4) or copies (None) the payload
        // into `output`, returning the declared memory_size.
        auto const materialized = storage_item.materialize(output);
        if (materialized.is_err()) {
            return ustl::err(materialized.unwrap_err());
        }
        auto const bootfs = bootfs::View::from(Bytes(output.data(), materialized.unwrap()));
        if (bootfs.is_err()) {
            return ustl::err(bootfs.unwrap_err());
        }
        return ustl::ok(KernelPackage(storage_item, bootfs.unwrap()));
    }

    /// The StorageKernel item this view was built from.
    [[nodiscard]]
    auto envelope() const -> obi::ItemView const & {
        return envelope_;
    }

    /// The validated BootFS view over the materialized payload.
    [[nodiscard]]
    auto bootfs() const -> bootfs::View const & {
        return bootfs_;
    }

    /// Find a file inside the package by name.
    [[nodiscard]]
    auto find(std::string_view name) const -> bootfs::View::FindResult {
        return bootfs_.find(name);
    }

    /// The bytes of a named file (e.g. the kernel ELF, code-patches).
    /// Reports NotFound when the name is absent.
    [[nodiscard]]
    auto file(std::string_view name) const -> FileResult {
        auto const found = bootfs_.find(name);
        if (found.is_err()) {
            return ustl::err(found.unwrap_err());
        }
        auto const entry = found.unwrap();
        if (entry.is_none()) {
            return ustl::err(ours::Status::NotFound);
        }
        return ustl::ok(bootfs_.file_data(entry.value()));
    }

  private:
    KernelPackage(obi::ItemView envelope, bootfs::View bootfs) : envelope_(envelope), bootfs_(bootfs) {}

    obi::ItemView envelope_;
    bootfs::View bootfs_;
};
} // namespace ours::phys

#endif // OURS_PHYS_KERNEL_PACKAGE_HPP
