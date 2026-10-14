/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_PROCESSOR_FEATURE_HPP
#define ARCH_X86_PROCESSOR_FEATURE_HPP 1

#include <ours/types.hpp>

namespace arch {

// This is intentionally the set consumed by the current x86 kernel.  Each
// member is populated from one well-defined CPUID register bit in init().
enum class CpuFeatureType : u8 {
    Pcid,
    X2Apic,
    TscDeadlineTimer,
    FsGsBase,
    Smep,
    InvPcid,
    Smap,
    Umip,
    La57,
    Monitor,
    Count,
};

class X86CpuFeature {
  public:
    auto init() -> void {
        m_bits = 0;

        u32 max_leaf = 0;
        u32 unused_ebx = 0;
        u32 unused_ecx = 0;
        u32 unused_edx = 0;
        cpuid(0, 0, max_leaf, unused_ebx, unused_ecx, unused_edx);

        if (max_leaf >= 1) {
            u32 eax = 0;
            u32 ebx = 0;
            u32 ecx = 0;
            u32 edx = 0;
            cpuid(1, 0, eax, ebx, ecx, edx);
            set(CpuFeatureType::Pcid, bit_is_set(ecx, 17));
            set(CpuFeatureType::X2Apic, bit_is_set(ecx, 21));
            set(CpuFeatureType::TscDeadlineTimer, bit_is_set(ecx, 24));
            set(CpuFeatureType::Monitor, bit_is_set(ecx, 3));
        }

        if (max_leaf >= 7) {
            u32 eax = 0;
            u32 ebx = 0;
            u32 ecx = 0;
            u32 edx = 0;
            cpuid(7, 0, eax, ebx, ecx, edx);
            set(CpuFeatureType::FsGsBase, bit_is_set(ebx, 0));
            set(CpuFeatureType::Smep, bit_is_set(ebx, 7));
            set(CpuFeatureType::InvPcid, bit_is_set(ebx, 10));
            set(CpuFeatureType::Smap, bit_is_set(ebx, 20));
            set(CpuFeatureType::Umip, bit_is_set(ecx, 2));
            set(CpuFeatureType::La57, bit_is_set(ecx, 16));
        }
    }

    [[nodiscard]]
    auto has_feature(CpuFeatureType feature) const -> bool {
        auto const index = static_cast<u8>(feature);
        return index < static_cast<u8>(CpuFeatureType::Count) && (m_bits & (u64{1} << index)) != 0;
    }

  private:
    static auto cpuid(u32 leaf, u32 subleaf, u32 &eax, u32 &ebx, u32 &ecx, u32 &edx) -> void {
#if defined(__i386__) || defined(__x86_64__)
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
#else
        (void)leaf;
        (void)subleaf;
        eax = 0;
        ebx = 0;
        ecx = 0;
        edx = 0;
#endif
    }

    static constexpr auto bit_is_set(u32 value, u8 bit) -> bool {
        return (value & (u32{1} << bit)) != 0;
    }

    auto set(CpuFeatureType feature, bool present) -> void {
        if (present) {
            m_bits |= u64{1} << static_cast<u8>(feature);
        }
    }

    u64 m_bits = 0;
};

using ArchCpuFeature = X86CpuFeature;

} // namespace arch

#endif // ARCH_X86_PROCESSOR_FEATURE_HPP
