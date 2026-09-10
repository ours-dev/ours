/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef ARCH_X86_SYSTEM_HPP
#define ARCH_X86_SYSTEM_HPP 1

#include <tuple>

#include <arch/sysreg.hpp>
#include <arch/types.hpp>
#include <ustl/bitfields.hpp>

namespace arch {

namespace detail {
    // Bit positions per [intel/vol3] 2.5 Control Registers. Kept as scoped
    // enums so each register's field list stays self-describing without
    // leaking bit names (Pe, Wp, ...) into arch:: at large; each register
    // class re-exposes its own enumerators via `using enum` so e.g.
    // `Cr0::Wp` keeps working exactly like before.
    //
    // These enum values are the single source of truth for bit position:
    // FieldAt<Id> below reads the position straight off the Id, so it's
    // never repeated (and can't drift out of sync) as a separate StartBit.
    enum class Cr0Bits : usize {
        Pe = 0,
        Mp = 1,
        Em = 2,
        Ts = 3,
        Et = 4,
        Ne = 5,
        Wp = 16,
        Am = 18,
        Nw = 29,
        Cd = 30,
        Pg = 31
    };
    enum class Cr2Bits : usize { Address = 0 };
    enum class Cr3Bits : usize { Pcid = 0, PageTableAddress = 12, NoFlush = 63 };
    enum class Cr4Bits : usize {
        Vme = 0,
        Pvi = 1,
        Tsd = 2,
        De = 3,
        Pse = 4,
        Pae = 5,
        Mce = 6,
        Pge = 7,
        Pce = 8,
        OsFxsr = 9,
        OsMmexcpt = 10,
        Umip = 11,
        La57 = 12,
        Vmxe = 13,
        Smxe = 14,
        FsGsBase = 16,
        Pcide = 17,
        OsxSave = 18,
        Smep = 20,
        Smap = 21,
        Pke = 22,
        Cet = 23,
        Pks = 24
    };

    // Every register here is a 64-bit-wide instruction target (`mov %crN,
    // %rax` moves the full 64 bits, even where the upper bits are reserved),
    // so the storage unit is pinned to `usize` explicitly rather than left
    // to BitFields' bit-count-driven default (which would pick a narrower
    // type for e.g. Cr0's 32 significant bits and break the ValueType match
    // SysRegIo's usize-returning read()/write() expect).
    using Cr0Layout = ustl::BitFields<
        ustl::bitfields::StorageUnit<usize>,
        std::tuple<
            ustl::FieldAt<Cr0Bits::Pe>, ustl::FieldAt<Cr0Bits::Mp>, ustl::FieldAt<Cr0Bits::Em>,
            ustl::FieldAt<Cr0Bits::Ts>, ustl::FieldAt<Cr0Bits::Et>, ustl::FieldAt<Cr0Bits::Ne>,
            ustl::FieldAt<Cr0Bits::Wp>, ustl::FieldAt<Cr0Bits::Am>, ustl::FieldAt<Cr0Bits::Nw>,
            ustl::FieldAt<Cr0Bits::Cd>, ustl::FieldAt<Cr0Bits::Pg>>>;

    using Cr2Layout = ustl::BitFields<ustl::bitfields::StorageUnit<usize>,
                                      std::tuple<ustl::FieldAt<Cr2Bits::Address, 64, usize>>>;

    using Cr3Layout = ustl::BitFields<
        ustl::bitfields::StorageUnit<usize>,
        std::tuple<ustl::FieldAt<Cr3Bits::Pcid, 12, usize>,
                   ustl::FieldAt<Cr3Bits::PageTableAddress, 40, usize, /*IsNatural=*/true>,
                   ustl::FieldAt<Cr3Bits::NoFlush>>>;

    using Cr4Layout = ustl::BitFields<
        ustl::bitfields::StorageUnit<usize>,
        std::tuple<
            ustl::FieldAt<Cr4Bits::Vme>, ustl::FieldAt<Cr4Bits::Pvi>, ustl::FieldAt<Cr4Bits::Tsd>,
            ustl::FieldAt<Cr4Bits::De>, ustl::FieldAt<Cr4Bits::Pse>, ustl::FieldAt<Cr4Bits::Pae>,
            ustl::FieldAt<Cr4Bits::Mce>, ustl::FieldAt<Cr4Bits::Pge>, ustl::FieldAt<Cr4Bits::Pce>,
            ustl::FieldAt<Cr4Bits::OsFxsr>, ustl::FieldAt<Cr4Bits::OsMmexcpt>,
            ustl::FieldAt<Cr4Bits::Umip>, ustl::FieldAt<Cr4Bits::La57>,
            ustl::FieldAt<Cr4Bits::Vmxe>, ustl::FieldAt<Cr4Bits::Smxe>,
            ustl::FieldAt<Cr4Bits::FsGsBase>, ustl::FieldAt<Cr4Bits::Pcide>,
            ustl::FieldAt<Cr4Bits::OsxSave>, ustl::FieldAt<Cr4Bits::Smep>,
            ustl::FieldAt<Cr4Bits::Smap>, ustl::FieldAt<Cr4Bits::Pke>, ustl::FieldAt<Cr4Bits::Cet>,
            ustl::FieldAt<Cr4Bits::Pks>>>;
} // namespace detail

class Cr0: public SysReg<Cr0, detail::Cr0Layout> {
  public:
    using enum detail::Cr0Bits;
};
ARCH_X86_SYSREG(Cr0, "cr0");

class Cr2: public SysReg<Cr2, detail::Cr2Layout> {
  public:
    using enum detail::Cr2Bits;
};
ARCH_X86_SYSREG(Cr2, "cr2");

class Cr3: public SysReg<Cr3, detail::Cr3Layout> {
    using Base = SysReg<Cr3, detail::Cr3Layout>;

  public:
    using enum detail::Cr3Bits;
    using Base::write;

    // Kept as a template (rather than a plain `write(PhysAddr)` overload)
    // so its body — which reaches into the SysRegIo<Cr3> hardware-access
    // path — is only instantiated when actually called. Cr0/Cr2/Cr4 don't
    // need this because they have no extra member of their own; here, an
    // ordinary (non-template) member's body is checked as soon as the
    // class is parsed, which would force SysRegIo<Cr3> to be resolved even
    // when nothing calls this overload (e.g. logic-only unit tests built
    // for a non-x86 host, where ARCH_X86_SYSREG never specializes it).
    template <typename Int = PhysAddr>
    static auto write(Int value) noexcept -> void {
        Base::write(from_value(static_cast<PhysAddr>(value)));
    }
};
ARCH_X86_SYSREG(Cr3, "cr3");

class Cr4: public SysReg<Cr4, detail::Cr4Layout> {
  public:
    using enum detail::Cr4Bits;
};
ARCH_X86_SYSREG(Cr4, "cr4");

} // namespace arch

#endif // ARCH_X86_SYSTEM_HPP
