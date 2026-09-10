/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_SYSREG_HPP
#define ARCH_SYSREG_HPP 1

#include <arch/io/register_base.hpp>

#include <ustl/traits/is_same.hpp>

namespace arch {

/// Each system register that's accessed via a dedicated CPU instruction
/// (x86 `mov` to/from cr0-cr4, aarch64 `mrs`/`msr`, ...) rather than a
/// numeric address is identified by its own C++ type — the "register tag",
/// normally the register description type itself. `SysRegIo<Tag>` is how
/// that tag's actual hardware access is plugged in: the primary template
/// below is a compile-time trap, and the `ARCH_*_SYSREG` macros define the
/// per-architecture specializations that do the real `read()`/`write()`.
///
/// (See `arch::io::RegisterBase` / `arch::io::RegisterAddr` for the
/// address-dispatched counterpart used by MSRs and MMIO registers, where
/// several registers share one access mechanism and differ only by a
/// numeric offset instead of by type.)
template <typename Tag>
struct SysRegIo {
    static auto read() -> usize {
        static_assert(!ustl::traits::IsSameV<Tag, Tag>,
                      "missing ARCH_*_SYSREG(...) for this register tag");
        return {};
    }

    static auto write(usize) -> void {
        static_assert(!ustl::traits::IsSameV<Tag, Tag>,
                      "missing ARCH_*_SYSREG(...) for this register tag");
    }
};

/// CRTP base for registers addressed by a dedicated instruction. `Layout`
/// is a `ustl::BitFields<Storage, Field<...>...>` describing the register's
/// bits; `RegisterLayout` (see io/register_base.hpp) turns that into
/// `get<Id>()`/`set<Id>()`/`value()`/`from_value()`, and this class adds
/// `read()`/`write()` on top, backed by a `SysRegIo<Derived>` specialization
/// installed via `ARCH_*_SYSREG(Derived, "regname")` right after Derived's
/// definition.
template <typename Derived, typename Layout>
class SysReg: public RegisterLayout<Derived, Layout> {
    using Base = RegisterLayout<Derived, Layout>;

  public:
    using typename Base::ValueType;

    static auto read() -> Derived {
        return Derived::from_value(SysRegIo<Derived>::read());
    }

    static auto write(Derived reg) -> void {
        SysRegIo<Derived>::write(reg.value());
    }

    auto write() -> Derived & {
        SysReg::write(static_cast<Derived &>(*this));
        return static_cast<Derived &>(*this);
    }
};

// <arch/system.hpp> uses this to declare x86 system registers. `Tag` is a
// unique C++ type deriving from `SysReg<Tag, Layout>`; `RegName` is the
// assembly register name without the leading '%'. Must be invoked inside
// `namespace arch { ... }`, right after Tag's definition.
//
// For example:
//   class Cr0 : public SysReg<Cr0, ...> { ... };
//   ARCH_X86_SYSREG(Cr0, "cr0");
#if defined(__x86_64__) || defined(__i386__)
#define ARCH_X86_SYSREG(Tag, RegName)                                                              \
    template <>                                                                                    \
    inline auto SysRegIo<Tag>::read() -> usize {                                                   \
        usize value;                                                                               \
        asm volatile("mov %%" RegName ", %0" : "=r"(value));                                       \
        return value;                                                                              \
    }                                                                                              \
    template <>                                                                                    \
    inline auto SysRegIo<Tag>::write(usize value) -> void {                                        \
        asm volatile("mov %0, %%" RegName : : "r"(value) : "memory");                              \
    }
#else
// Not callable on non-x86; using it (via SysReg<Tag,...>::read/write) will
// fail with the static_assert in the primary SysRegIo template above.
#define ARCH_X86_SYSREG(Tag, RegName)
#endif

} // namespace arch

#endif // #ifndef ARCH_SYSREG_HPP
