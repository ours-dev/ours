/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <eftl/binary.hpp>

namespace {
class FakeBinary final: public eftl::Binary {
  public:
    [[nodiscard]]
    auto format() const noexcept -> eftl::Binary::FormatId override {
        return eftl::Binary::FormatId::Elf;
    }

    [[nodiscard]]
    auto entrypoint() const noexcept -> u64 override {
        return 0x401000;
    }

    [[nodiscard]]
    auto imagebase() const noexcept -> u64 override {
        return 0x400000;
    }

    [[nodiscard]]
    auto virtual_address_to_offset(u64 virtual_address) const noexcept
        -> ustl::Option<usize> override {
        if (virtual_address == 0x401000) {
            return ustl::some(usize{0x1000});
        }
        return ustl::NONE;
    }
};
} // namespace

auto main() -> int {
    FakeBinary const fake_binary;
    eftl::Binary const &binary = fake_binary;

    if (binary.format() != eftl::Binary::FormatId::Elf) {
        return 1;
    }
    if (binary.entrypoint() != 0x401000) {
        return 2;
    }
    if (binary.imagebase() != 0x400000) {
        return 3;
    }
    auto const offset = binary.virtual_address_to_offset(0x401000);
    if (offset.is_none() || offset.value() != 0x1000) {
        return 4;
    }
    if (binary.virtual_address_to_offset(0x400000).is_some()) {
        return 5;
    }
    return 0;
}
