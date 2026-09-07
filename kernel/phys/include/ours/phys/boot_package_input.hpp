/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BOOT_PACKAGE_INPUT_HPP
#define OURS_PHYS_BOOT_PACKAGE_INPUT_HPP 1

#include <ours/phys/arch_bootmem.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <limits>

namespace ours::phys {
/// A validated physical OBI package range supplied by the boot loader.
/// `init_memory` reserves it immediately after memory discovery and before
/// any BootMem allocation can overwrite its nested payloads.
class BootPackageInput final {
  public:
    using Result = ustl::Result<BootPackageInput, Status>;

    [[nodiscard]]
    static auto from(PhysAddr const base, usize const size) -> Result {
        if (base == 0 || size == 0) {
            return ustl::err(Status::InvArgs);
        }
        if (base > std::numeric_limits<PhysAddr>::max() - size) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(BootPackageInput(base, size));
    }

    [[nodiscard]]
    auto bytes() const -> ustl::views::Span<u8 const> {
        return {reinterpret_cast<u8 const *>(base_), size_};
    }

    [[nodiscard]]
    auto reserve(BootMem &memory) const -> Status {
        auto const range = bootmem::Range::from(base_, size_);
        if (range.is_err()) {
            return range.unwrap_err();
        }
        return memory.reserve(range.unwrap());
    }

  private:
    BootPackageInput(PhysAddr const base, usize const size) : base_(base), size_(size) {}

    PhysAddr base_;
    usize size_;
};
} // namespace ours::phys

#endif // OURS_PHYS_BOOT_PACKAGE_INPUT_HPP
