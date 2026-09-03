/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_FEATURES_HPP
#define OURS_ARCH_X86_FEATURES_HPP 1

#include <arch/x86/processor/feature.hpp>

namespace ours {
using arch::CpuFeatureType;

auto x86_has_feature(CpuFeatureType type) -> bool;

extern bool FEATURE_HAS_FSGSBASE;

} // namespace ours

#endif // #ifndef OURS_ARCH_X86_FEATURES_HPP