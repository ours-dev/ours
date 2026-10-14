/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_FEATURES_HPP
#define OURS_ARCH_X86_FEATURES_HPP 1

#include <arch/x86/processor/feature.hpp>

namespace ours {
using arch::CpuFeatureType;

auto x86_has_feature(CpuFeatureType type) -> bool;

extern bool FEATURE_HAS_FSGSBASE;

/// Whether CR4.SMAP is set -- x86_init_mmu_percpu() sets it on exactly the
/// CPUs that report the feature, so the two are the same fact.
///
/// Cached rather than asked of x86_has_feature() because the question comes up
/// on every user copy, and x86_has_feature() executes CPUID, which a
/// hypervisor traps. It is also the answer to "may this kernel execute stac at
/// all": on a CPU without SMAP, stac and clac are #UD.
extern bool FEATURE_HAS_SMAP;

/// Whether the CPU has MONITOR and MWAIT, so that an idle CPU waits in MWAIT
/// on a word a waker writes, instead of in HLT for an interrupt
/// (ArchIdleThread). Cached for the reason FEATURE_HAS_SMAP is: the idle loop
/// and every reschedule ask.
extern bool FEATURE_HAS_MONITOR;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_FEATURES_HPP