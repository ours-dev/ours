/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_OBI_HPP
#define OURS_PHYS_BOOT_OBI_HPP 1

#include <ours/phys/kernel_package.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <obi/command_line_view.hpp>
#include <obi/container_view.hpp>
#include <obi/memory_config_view.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
/// A view over the object a `kernel_image` GN template produces: a
/// bootable OBI container (the whole `.obi`).  It validates the boot
/// contract — a required StorageKernel kernel package — and provides
/// typed access to the command line, memory config, and kernel package.
class BootObi final {
  public:
    using Bytes = ustl::views::Span<u8 const>;
    using MutableBytes = ustl::views::Span<u8>;
    using Result = ustl::Result<BootObi, ours::Status>;
    using KernelPackageResult = ustl::Result<KernelPackage, ours::Status>;
    using KernelPackageItemResult = ustl::Result<obi::ItemView, ours::Status>;

    /// Validate the boot contract: the container must carry exactly one
    /// StorageKernel item (the kernel package).  Zero is a broken image;
    /// more than one means the packer split the kernel packages and the
    /// boot chain would silently depend on item order.
    [[nodiscard]]
    static auto from(obi::ContainerView const &container) -> Result {
        usize kernel_packages = 0;
        for (usize index = 0; index < container.item_count(); ++index) {
            auto const item = container.item(index);
            if (item.is_err()) {
                return ustl::err(item.unwrap_err());
            }
            if (item.unwrap().type() == obi::ItemType::StorageKernel) {
                kernel_packages += 1;
            }
        }
        if (kernel_packages == 0) {
            return ustl::err(Status::NotFound);
        }
        if (kernel_packages > 1) {
            return ustl::err(Status::AlreadyExists);
        }
        return ustl::ok(BootObi(container));
    }

    /// The underlying container.
    [[nodiscard]]
    auto package() const -> obi::ContainerView const & {
        return package_;
    }

    /// The Executable entry-point item (index 0 by PackageBuilder's
    /// sort order) — the object the boot shim loads first, e.g. the
    /// physload ELF.  None when the container has no such item.
    [[nodiscard]]
    auto executable() const -> ustl::Option<obi::ItemView> {
        if (package_.item_count() == 0) {
            return ustl::NONE;
        }
        auto const item = package_.item(0);
        if (item.is_err()) {
            return ustl::NONE;
        }
        auto const &view = item.unwrap();
        if (view.type() != obi::ItemType::Executable) {
            return ustl::NONE;
        }
        return ustl::Option<obi::ItemView>(view);
    }

    /// The raw StorageKernel item, before materialization.  Lets a caller
    /// that needs to size its own decompression buffer (e.g. ModuleLoader,
    /// which allocates the buffer out of bootmem) read `memory_size()`
    /// first; kernel_package() below is for callers that already have one.
    [[nodiscard]]
    auto kernel_package_item() const -> KernelPackageItemResult {
        for (usize index = 0; index < package_.item_count(); ++index) {
            auto const item = package_.item(index);
            if (item.is_err()) {
                return ustl::err(item.unwrap_err());
            }
            if (item.unwrap().type() == obi::ItemType::StorageKernel) {
                return ustl::ok(item.unwrap());
            }
        }
        return ustl::err(ours::Status::NotFound);
    }

    /// The kernel package (StorageKernel BootFS).  `buffer` receives the
    /// decompressed BootFS and must outlive the returned KernelPackage.
    [[nodiscard]]
    auto kernel_package(MutableBytes buffer) const -> KernelPackageResult {
        auto const item = kernel_package_item();
        if (item.is_err()) {
            return ustl::err(item.unwrap_err());
        }
        return KernelPackage::from(item.unwrap(), buffer);
    }

    /// The command line (Config/Raw item), if present.
    [[nodiscard]]
    auto command_line() const -> ustl::Option<obi::CommandLineView> {
        return find_view<obi::CommandLineView>(obi::ItemType::Config, obi::ItemFormat::Raw);
    }

    /// The memory config (Data/MemConfig item), if present.
    [[nodiscard]]
    auto memory_config() const -> ustl::Option<obi::MemoryConfigView> {
        return find_view<obi::MemoryConfigView>(obi::ItemType::Data, obi::ItemFormat::MemConfig);
    }

  private:
    explicit BootObi(obi::ContainerView package) : package_(package) {}

    /// Scans for the first item of (type, format) that also parses as
    /// View; skips items that fail to parse rather than reporting the
    /// error, on the theory that "no config/memory item present" and
    /// "one is present but malformed" both just mean "nothing usable
    /// here" to command_line()/memory_config()'s callers.
    template <typename View>
    [[nodiscard]]
    auto find_view(obi::ItemType const type, obi::ItemFormat const format) const
        -> ustl::Option<View> {
        for (usize index = 0; index < package_.item_count(); ++index) {
            auto const item = package_.item(index);
            if (item.is_err()) {
                continue;
            }
            auto const &view = item.unwrap();
            if (view.type() != type || view.format() != format) {
                continue;
            }
            auto const parsed = View::from(view);
            if (parsed.is_ok()) {
                return ustl::Option<View>(parsed.unwrap());
            }
        }
        return ustl::NONE;
    }

    obi::ContainerView package_;
};
} // namespace ours::phys

#endif // OURS_PHYS_BOOT_OBI_HPP
