/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>

#include <arch/x86/system.hpp>
#include <arch/x86/msr.hpp>

namespace {

constexpr auto control_register_values_are_masked() -> bool {
    auto cr0 = arch::Cr0::from_value(0).set<arch::Cr0::Wp>(true).set<arch::Cr0::Cd>(true);
    auto cr3 = arch::Cr3::from_value(0)
                   .set<arch::Cr3::Pcid>(0xffff)
                   .set<arch::Cr3::PageTableAddress>(0x1234'5678'9000ULL)
                   .set<arch::Cr3::NoFlush>(true);
    auto cr4 = arch::Cr4::from_value(0).set<arch::Cr4::Pge>(true).set<arch::Cr4::Smap>(true);

    return cr0.get<arch::Cr0::Wp>() && cr0.get<arch::Cr0::Cd>() && cr3.get<arch::Cr3::Pcid>() == 0xfff
           && cr3.get<arch::Cr3::PageTableAddress>() == 0x1234'5678'9000ULL
           && cr3.get<arch::Cr3::NoFlush>() && cr4.get<arch::Cr4::Pge>() && cr4.get<arch::Cr4::Smap>();
}

static_assert(control_register_values_are_masked());

constexpr auto efer_bits_round_trip() -> bool {
    auto efer = arch::X86Efer::from_value(0).set<arch::X86Efer::Sce>(true).set<arch::X86Efer::Nxe>(true);
    return efer.get<arch::X86Efer::Sce>() && !efer.get<arch::X86Efer::Lme>() && efer.get<arch::X86Efer::Nxe>();
}

static_assert(efer_bits_round_trip());

} // namespace

auto main() -> int {
    auto cr3 = arch::Cr3::from_value(0).set<arch::Cr3::PageTableAddress>(0x4000ULL);
    assert(cr3.value() == 0x4000ULL);
}
