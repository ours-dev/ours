/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_MSR_HPP
#define ARCH_X86_MSR_HPP 1

#include <arch/io/register_addr.hpp>
#include <arch/types.hpp>

#include <tuple>

#include <ustl/bitfields.hpp>

namespace arch {
enum class MsrRegAddr : u32 {
    IA32ApicBase = 0x1B,
    IA32Pmc0 = 0xC1,
    IA32PerfEvtSel0 = 0x186,
    IA32PerfCapabilities = 0x341,
    IA32Efer = 0xC000'0080,
    IA32Star = 0xC000'0081,
    IA32Lstar = 0xC000'0082,
    IA32Fmask = 0xC000'0084,
    IA32FsBase = 0xC000'0100,
    IA32GsBase = 0xC000'0101,
    IA32KernelGsBase = 0xC000'0102,
    IA32TscDeadline = 0x6E0,
    IA32PerfGlobalStatus = 0x38E,
    IA32PerfGlobalCtrl = 0x38F,
    IA32PerfGlobalOvfCtrl = 0x390,
};

class MsrIo final {
  public:
    template <typename Int>
    static auto write(MsrRegAddr address, Int value) noexcept -> void {
#if defined(__i386__) || defined(__x86_64__)
        auto const raw = static_cast<u64>(value);
        auto const low = static_cast<u32>(raw);
        auto const high = static_cast<u32>(raw >> 32);
        asm volatile("wrmsr" : : "c"(static_cast<u32>(address)), "a"(low), "d"(high));
#else
        (void)address;
        (void)value;
#endif
    }

    template <typename Int = u64>
    [[nodiscard]]
    static auto read(MsrRegAddr address) noexcept -> Int {
#if defined(__i386__) || defined(__x86_64__)
        u32 low;
        u32 high;
        asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(static_cast<u32>(address)));
        return static_cast<Int>((static_cast<u64>(high) << 32) | low);
#else
        (void)address;
        return {};
#endif
    }

    // Instance methods shaped to the `IoProvider` interface expected by
    // `arch::io::RegisterBase::read_from/write_to` (addr as `u32`, value
    // read/written by-value), so a plain `MsrIo{}` can drive a typed
    // `io::RegisterBase`-derived MSR register (see X86Efer below). They
    // just forward to the static API above.
    template <typename Int>
    auto read(u32 addr) const -> Int {
        return read<Int>(static_cast<MsrRegAddr>(addr));
    }

    template <typename Int>
    auto write(Int value, u32 addr) const -> void {
        write(static_cast<MsrRegAddr>(addr), value);
    }
};

namespace detail {
    enum class EferBits : usize { Sce = 0, Lme = 8, Lma = 10, Nxe = 11 };

    using EferLayout =
        ustl::BitFields<ustl::bitfields::StorageUnit<usize>,
                        std::tuple<ustl::FieldAt<EferBits::Sce>, ustl::FieldAt<EferBits::Lme>,
                                   ustl::FieldAt<EferBits::Lma>, ustl::FieldAt<EferBits::Nxe>>>;
} // namespace detail

// [intel/sdm] 2.2.1 Extended Feature Enable Register (IA32_EFER). `Lma` is
// a status bit set by the processor (long mode active); it's exposed for
// `get<>()` like the others but writing it has no defined effect.
class X86Efer: public io::RegisterBase<X86Efer, detail::EferLayout> {
    using Base = io::RegisterBase<X86Efer, detail::EferLayout>;

  public:
    using enum detail::EferBits;

    static auto read() -> X86Efer {
        MsrIo io;
        return io::RegisterAddr<X86Efer>(static_cast<u32>(MsrRegAddr::IA32Efer)).read_from(io);
    }

    auto write() -> X86Efer & {
        Base::set_addr(static_cast<u32>(MsrRegAddr::IA32Efer));
        MsrIo io;
        Base::write_to(io);
        return *this;
    }
};

} // namespace arch

#endif // ARCH_X86_MSR_HPP
