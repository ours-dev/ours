/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <user_copy/copy_allowed.hpp>
#include <user_copy/range.hpp>

#include <limits>

#include <gtest/gtest.h>

namespace {
using user_copy::is_within;

constexpr ours::VirtAddr BASE = 0x20'0000;
constexpr usize SPAN = (usize{1} << 47) - 0x1000 - BASE;

TEST(IsWithin, WholeRangeInside) {
    EXPECT_TRUE(is_within(BASE, 0x1000, BASE, SPAN));
    EXPECT_TRUE(is_within(BASE + SPAN - 1, 1, BASE, SPAN));
}

TEST(IsWithin, EndsExactlyAtTheLimit) {
    EXPECT_TRUE(is_within(BASE + SPAN - 0x1000, 0x1000, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + SPAN - 0x1000, 0x1001, BASE, SPAN));
}

TEST(IsWithin, NullAndTheHoleBelowTheBase) {
    EXPECT_FALSE(is_within(0, 1, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE - 1, 1, BASE, SPAN));
    EXPECT_FALSE(is_within(0, 0, BASE, SPAN));
}

TEST(IsWithin, KernelHalf) {
    EXPECT_FALSE(is_within(0xffff'8000'0000'0000, 1, BASE, SPAN));
}

TEST(IsWithin, SizeThatWouldWrapTheSum) {
    // `addr + size` wraps to a small number here; a naive `end <= limit` check
    // would accept it.
    EXPECT_FALSE(is_within(BASE + 0x1000, std::numeric_limits<usize>::max(), BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + 0x1000, std::numeric_limits<usize>::max() - 0xfff, BASE, SPAN));
}

TEST(IsWithin, EmptyRangeMustStillStartInside) {
    EXPECT_TRUE(is_within(BASE, 0, BASE, SPAN));
    EXPECT_FALSE(is_within(BASE + SPAN, 0, BASE, SPAN));
}

// CopyAllowed is decided at compile time, so its cases are static_asserts:
// this file failing to build is the test failing.
namespace copy_allowed {
    using user_copy::CopyAllowed;

    /// The hole: |base| is 8-aligned, so 4 bytes of padding follow |kind|,
    /// and sizeof is 16, not 12.
    struct Padded {
        u32 kind;
        u64 base;
    };
    static_assert(sizeof(Padded) == 16);
    static_assert(!CopyAllowed<Padded>);

    /// The same struct with the hole named, as Zircon's info structs do.
    struct PaddingNamed {
        u32 kind;
        u8 padding1[4];
        u64 base;
    };
    static_assert(CopyAllowed<PaddingNamed>);

    /// Not trivially copyable: a copy has to set up the vtable pointer.
    struct Polymorphic {
        virtual ~Polymorphic();
        u64 value;
    };
    static_assert(!CopyAllowed<Polymorphic>);

    /// Not standard layout: fields in both a base and the derived class, so
    /// where |b| sits is the compiler's choice, not a C declaration's.
    struct Base {
        u32 a;
    };
    struct Derived: Base {
        u32 b;
    };
    static_assert(!CopyAllowed<Derived>);

    /// Allowed, unlike under Zircon's is_trivial: a default constructor of
    /// its own says nothing about whether its bytes may be copied.
    struct WithConstructor {
        WithConstructor();
        u32 value;
    };
    static_assert(CopyAllowed<WithConstructor>);

    /// +0.0 and -0.0 are equal with different bytes, so a float has no unique
    /// representation and is refused. No system call copies one.
    static_assert(!CopyAllowed<float>);

    // What the system calls copy today.
    static_assert(CopyAllowed<u8>);
    static_assert(CopyAllowed<char>);
    static_assert(CopyAllowed<ours::HandleValue>);
    static_assert(CopyAllowed<ours::VirtAddr>);
} // namespace copy_allowed

} // namespace
