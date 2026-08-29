/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#pragma once

#include <ours/types.hpp>

#include <type_traits>

namespace arch {
    /// Runs the CPUID leaf an observer names and hands it the four registers.
    ///
    /// An observer is any type with a LEAF, a SUBLEAF and an observe() -- the
    /// leaf is part of the type, so the caller never repeats it and cannot pair
    /// a leaf with the wrong reader.
    ///
    /// TODO(ours): only the address-size observer below exists so far. The
    /// feature observers this header was reserved for are still to come, and
    /// arch::CpuFeature reads its own leaves directly in the meantime.
    template <typename Observer>
    inline auto dispatch_cpuid(Observer &&observer) -> void {
#if defined(__i386__) || defined(__x86_64__)
        using Traits = std::remove_reference_t<Observer>;

        auto const raw = [](u32 leaf, u32 subleaf, u32 &eax, u32 &ebx, u32 &ecx, u32 &edx) {
            asm volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf), "c"(subleaf)
                         : "memory");
        };

        u32 eax = 0, ebx = 0, ecx = 0, edx = 0;

        // An extended leaf the processor does not implement does not fault --
        // it answers as though the highest supported leaf had been asked for,
        // which looks like a plausible result. Leaf 0x8000'0000 reports how far
        // the extended range actually goes, so ask that first and leave the
        // observer untouched if the leaf is out of range.
        if constexpr (Traits::LEAF >= 0x80000000U) {
            raw(0x80000000U, 0, eax, ebx, ecx, edx);
            if (eax < Traits::LEAF) {
                return;
            }
        }

        raw(Traits::LEAF, Traits::SUBLEAF, eax, ebx, ecx, edx);
        observer.observe(eax, ebx, ecx, edx);
#else
        (void)observer;
#endif
    }

    /// The address widths the processor actually implements, from CPUID leaf
    /// 0x8000'0008.
    ///
    /// Field layout follows Zircon's CpuidAddressSizeInfo
    /// (lib/arch/include/lib/arch/x86/cpuid.h), which cites
    /// [intel/vol2] Table 3-8 and [amd/vol3] E.4.7: EAX carries the physical
    /// address width in bits [7:0] and the linear (virtual) one in bits [15:8].
    ///
    /// Leaves the outputs alone when the leaf is unavailable, so whatever
    /// conservative defaults the caller started with survive.
    class AddrSizeCpuIdObserver {
      public:
        constexpr static u32 LEAF = 0x80000008U;
        constexpr static u32 SUBLEAF = 0U;

        AddrSizeCpuIdObserver(u8 &phys_addr_bits, u8 &virt_addr_bits)
            : m_phys_addr_bits(phys_addr_bits), m_virt_addr_bits(virt_addr_bits) {}

        auto observe(u32 eax, u32 ebx, u32 ecx, u32 edx) const -> void {
            static_cast<void>(ebx);
            static_cast<void>(ecx);
            static_cast<void>(edx);

            if (auto const phys = static_cast<u8>(eax & 0xFFU)) {
                m_phys_addr_bits = phys;
            }
            if (auto const virt = static_cast<u8>((eax >> 8) & 0xFFU)) {
                m_virt_addr_bits = virt;
            }
        }

      private:
        u8 &m_phys_addr_bits;
        u8 &m_virt_addr_bits;
    };

} // namespace arch
