/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>
#include <cstdint>

#include <ustl/util/enum_bits.hpp>

namespace enum_bits_test {
enum class Flags : std::uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
};
USTL_ENABLE_ENUM_BITMASK(Flags);

struct Holder {
    enum class Nested : unsigned {
        None = 0,
        First = 1,
        Second = 2,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Nested);
};

enum class Plain : unsigned {
    Only = 1,
};

static_assert(UstlEnumBitMaskEnabledV<Flags>);
static_assert(UstlEnumBitMaskEnabledV<Holder::Nested>);
static_assert(!UstlEnumBitMaskEnabledV<Plain>);
static_assert((Flags::Read | Flags::Write) == static_cast<Flags>(3));
static_assert((static_cast<Flags>(3) & Flags::Write) == Flags::Write);
static_assert(~Flags::Read == static_cast<Flags>(~std::uint8_t{1}));
static_assert(!Flags::None);
static_assert(!!Flags::Read);
} // namespace enum_bits_test

auto main() -> int {
    auto flags = enum_bits_test::Flags::Read;
    flags |= enum_bits_test::Flags::Write;
    assert(flags == static_cast<enum_bits_test::Flags>(3));
    flags &= enum_bits_test::Flags::Read;
    assert(flags == enum_bits_test::Flags::Read);

    auto nested = enum_bits_test::Holder::Nested::First | enum_bits_test::Holder::Nested::Second;
    assert(nested == static_cast<enum_bits_test::Holder::Nested>(3));
}
