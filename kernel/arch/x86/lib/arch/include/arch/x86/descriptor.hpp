/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_DESCRIPTOR_HPP
#define ARCH_X86_DESCRIPTOR_HPP 1

#include <arch/types.hpp>

// PACKED below comes from here. The header used to rely on some earlier
// include pulling it in, so it only compiled when it happened to be included
// after one -- which include sorting no longer guarantees.
#include <cstddef>

#include <ours/compiler.hpp>

namespace arch {
enum class SegType : u8 {
    Tss64Available = 0x9,
    Tss64Busy = 0xb,
};

enum class Dpl : u8 {
    Ring0 = 0,
    Ring1 = 1,
    Ring2 = 2,
    Ring3 = 3,
};

enum class SegGran : u8 {
    Byte = 0,
    Page = 1,
};

inline constexpr auto MAX_IO_BITMAP_BYTES = usize{8192};

[[nodiscard]]
constexpr auto to_string(SegType const type) -> char const * {
    switch (type) {
        case SegType::Tss64Available:
            return "TSS64 available";
        case SegType::Tss64Busy:
            return "TSS64 busy";
    }
    return "unknown";
}

[[nodiscard]]
constexpr auto to_string(Dpl const privilege) -> char const * {
    switch (privilege) {
        case Dpl::Ring0:
            return "ring 0";
        case Dpl::Ring1:
            return "ring 1";
        case Dpl::Ring2:
            return "ring 2";
        case Dpl::Ring3:
            return "ring 3";
    }
    return "unknown";
}

struct PACKED DescPtr final {
    u16 limit;
    usize base;
};
static_assert(sizeof(DescPtr) == 10);

inline auto lgdt(DescPtr const &descriptor) -> void {
    asm volatile("lgdt %0" : : "m"(descriptor) : "memory");
}

inline auto ltr(u16 const selector) -> void {
    asm volatile("ltr %0" : : "rm"(selector) : "memory");
}

struct PACKED GdtDesc32 {
    using Self = GdtDesc32;

    [[nodiscard]]
    constexpr auto base() const -> usize {
        return static_cast<usize>(m_base_low) | (static_cast<usize>(m_base_middle) << 16) |
               (static_cast<usize>(m_base_high) << 24);
    }

    [[nodiscard]]
    constexpr auto limit() const -> usize {
        return static_cast<usize>(m_limit_low) |
               (static_cast<usize>(m_limit_high_and_flags & LIMIT_HIGH_MASK) << 16);
    }

    [[nodiscard]]
    constexpr auto type() const -> SegType {
        return static_cast<SegType>(m_access & TYPE_MASK);
    }

    [[nodiscard]]
    constexpr auto dpl() const -> Dpl {
        return static_cast<Dpl>((m_access >> DPL_SHIFT) & DPL_MASK);
    }

    constexpr auto set_base(usize const value) -> Self & {
        m_base_low = static_cast<u16>(value);
        m_base_middle = static_cast<u8>(value >> 16);
        m_base_high = static_cast<u8>(value >> 24);
        return *this;
    }

    constexpr auto set_limit(usize const value) -> Self & {
        m_limit_low = static_cast<u16>(value);
        m_limit_high_and_flags = static_cast<u8>((m_limit_high_and_flags & ~LIMIT_HIGH_MASK) |
                                                 ((value >> 16) & LIMIT_HIGH_MASK));
        return *this;
    }

    constexpr auto set_type(SegType const value) -> Self & {
        m_access = static_cast<u8>((m_access & ~TYPE_MASK) | static_cast<u8>(value));
        return *this;
    }

    constexpr auto set_present(bool const value) -> Self & {
        m_access = value ? static_cast<u8>(m_access | PRESENT_MASK)
                         : static_cast<u8>(m_access & ~PRESENT_MASK);
        return *this;
    }

    constexpr auto set_dpl(Dpl const value) -> Self & {
        m_access = static_cast<u8>((m_access & ~(DPL_MASK << DPL_SHIFT)) |
                                   (static_cast<u8>(value) << DPL_SHIFT));
        return *this;
    }

    constexpr auto set_granularity(SegGran const value) -> Self & {
        m_limit_high_and_flags = value == SegGran::Page
                                     ? static_cast<u8>(m_limit_high_and_flags | GRANULARITY_MASK)
                                     : static_cast<u8>(m_limit_high_and_flags & ~GRANULARITY_MASK);
        return *this;
    }

    constexpr auto set_longmode(bool const value) -> Self & {
        m_limit_high_and_flags = value ? static_cast<u8>(m_limit_high_and_flags | LONG_MODE_MASK)
                                       : static_cast<u8>(m_limit_high_and_flags & ~LONG_MODE_MASK);
        return *this;
    }

  private:
    static constexpr auto TYPE_MASK = u8{0x0f};
    static constexpr auto DPL_MASK = u8{0x03};
    static constexpr auto DPL_SHIFT = u8{5};
    static constexpr auto PRESENT_MASK = u8{1} << 7;
    static constexpr auto LIMIT_HIGH_MASK = u8{0x0f};
    static constexpr auto LONG_MODE_MASK = u8{1} << 5;
    static constexpr auto GRANULARITY_MASK = u8{1} << 7;

    u16 m_limit_low{};
    u16 m_base_low{};
    u8 m_base_middle{};
    u8 m_access{};
    u8 m_limit_high_and_flags{};
    u8 m_base_high{};
};
static_assert(sizeof(GdtDesc32) == 8);

struct PACKED GdtDesc64 final {
    using Self = GdtDesc64;

    [[nodiscard]]
    constexpr auto base() const -> usize {
        return m_low.base() | (static_cast<usize>(m_base_upper) << 32);
    }

    [[nodiscard]]
    constexpr auto limit() const -> usize {
        return m_low.limit();
    }

    [[nodiscard]]
    constexpr auto type() const -> SegType {
        return m_low.type();
    }

    [[nodiscard]]
    constexpr auto dpl() const -> Dpl {
        return m_low.dpl();
    }

    constexpr auto set_limit(usize const value) -> Self & {
        m_low.set_limit(value);
        return *this;
    }

    constexpr auto set_type(SegType const value) -> Self & {
        m_low.set_type(value);
        return *this;
    }

    constexpr auto set_present(bool const value) -> Self & {
        m_low.set_present(value);
        return *this;
    }

    constexpr auto set_dpl(Dpl const value) -> Self & {
        m_low.set_dpl(value);
        return *this;
    }

    constexpr auto set_granularity(SegGran const value) -> Self & {
        m_low.set_granularity(value);
        return *this;
    }

    constexpr auto set_longmode(bool const value) -> Self & {
        m_low.set_longmode(value);
        return *this;
    }

    constexpr auto set_base(usize const value) -> Self & {
        m_low.set_base(value);
        m_base_upper = static_cast<u32>(value >> 32);
        return *this;
    }

  private:
    GdtDesc32 m_low{};
    u32 m_base_upper{};
    [[maybe_unused]]
    u32 m_reserved{};
};
static_assert(sizeof(GdtDesc64) == 16);

struct PACKED TaskStateSegment64 {
    u32 reserved0{};
    u64 sp0{};
    u64 sp1{};
    u64 sp2{};
    u64 reserved1{};
    u64 ist1{};
    u64 ist2{};
    u64 ist3{};
    u64 ist4{};
    u64 ist5{};
    u64 ist6{};
    u64 ist7{};
    u64 reserved2{};
    u16 reserved3{};
    u16 io_port_bitmap_base{};
};
static_assert(sizeof(TaskStateSegment64) == 104);
static_assert(offsetof(TaskStateSegment64, sp0) == 4);
static_assert(offsetof(TaskStateSegment64, sp2) == 20);
} // namespace arch

#endif // ARCH_X86_DESCRIPTOR_HPP
