/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_BOOT_OBI_HPP
#define OBI_BOOT_OBI_HPP 1

#include <obi/command-line-view.hpp>
#include <obi/container-view.hpp>
#include <obi/kernel-package.hpp>
#include <obi/memory-config-view.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace obi {
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

    /// Validate the boot contract: the container must carry exactly one
    /// StorageKernel item (the kernel package).  Zero is a broken image;
    /// more than one means the packer split the kernel packages and the
    /// boot chain would silently depend on item order.
    [[nodiscard]]
    static auto from(ContainerView const &container) -> Result {
        usize kernel_packages = 0;
        for (usize index = 0; index < container.item_count(); ++index) {
            auto const item = container.item(index);
            if (item.is_err()) {
                return ustl::err(item.unwrap_err());
            }
            if (item.unwrap().type() == ItemType::StorageKernel) {
                kernel_packages += 1;
            }
        }
        if (kernel_packages == 0) {
            return ustl::err(ours::Status::NotFound);
        }
        if (kernel_packages > 1) {
            return ustl::err(ours::Status::AlreadyExists);
        }
        return ustl::ok(BootObi(container));
    }

    /// The underlying container.
    [[nodiscard]]
    auto package() const -> ContainerView const & {
        return package_;
    }

    /// The Executable entry-point item (index 0 by PackageBuilder's
    /// sort order) — the object the boot shim loads first, e.g. the
    /// physload ELF.  None when the container has no such item.
    [[nodiscard]]
    auto executable() const -> ustl::Option<ItemView> {
        if (package_.item_count() == 0) {
            return ustl::NONE;
        }
        auto const item = package_.item(0);
        if (item.is_err()) {
            return ustl::NONE;
        }
        auto const &view = item.unwrap();
        if (view.type() != ItemType::Executable) {
            return ustl::NONE;
        }
        return ustl::Option<ItemView>(view);
    }

    /// The kernel package (StorageKernel BootFS).  `buffer` receives the
    /// decompressed BootFS and must outlive the returned KernelPackage.
    [[nodiscard]]
    auto kernel_package(MutableBytes buffer) const -> KernelPackageResult {
        for (usize index = 0; index < package_.item_count(); ++index) {
            auto const item = package_.item(index);
            if (item.is_err()) {
                return ustl::err(item.unwrap_err());
            }
            if (item.unwrap().type() == ItemType::StorageKernel) {
                return KernelPackage::from(item.unwrap(), buffer);
            }
        }
        return ustl::err(ours::Status::NotFound);
    }

    /// The command line (Config/Raw item), if present.
    [[nodiscard]]
    auto command_line() const -> ustl::Option<CommandLineView> {
        for (usize index = 0; index < package_.item_count(); ++index) {
            auto const item = package_.item(index);
            if (item.is_err()) {
                continue;
            }
            auto const &view = item.unwrap();
            if (view.type() != ItemType::Config || view.format() != ItemFormat::Raw) {
                continue;
            }
            auto const cmdline = CommandLineView::from(view);
            if (cmdline.is_ok()) {
                return ustl::Option<CommandLineView>(cmdline.unwrap());
            }
        }
        return ustl::NONE;
    }

    /// The memory config (Data/MemConfig item), if present.
    [[nodiscard]]
    auto memory_config() const -> ustl::Option<MemoryConfigView> {
        for (usize index = 0; index < package_.item_count(); ++index) {
            auto const item = package_.item(index);
            if (item.is_err()) {
                continue;
            }
            auto const &view = item.unwrap();
            if (view.type() != ItemType::Data || view.format() != ItemFormat::MemConfig) {
                continue;
            }
            auto const memcfg = MemoryConfigView::from(view);
            if (memcfg.is_ok()) {
                return ustl::Option<MemoryConfigView>(memcfg.unwrap());
            }
        }
        return ustl::NONE;
    }

  private:
    explicit BootObi(ContainerView package) : package_(package) {}

    ContainerView package_;
};
} // namespace obi

#endif // OBI_BOOT_OBI_HPP
