/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_X86_PROCESSOR_HPP
#define ARCH_X86_PROCESSOR_HPP 1

#include <arch/x86/processor/feature.hpp>

namespace arch {

struct X86CpuInfo {
    auto init() -> void {
        features_.init();
    }

    [[nodiscard]]
    auto feature() -> X86CpuFeature & {
        return features_;
    }

    [[nodiscard]]
    auto feature() const -> X86CpuFeature const & {
        return features_;
    }

  private:
    X86CpuFeature features_;
};

using ArchCpuInfo = X86CpuInfo;

} // namespace arch

#endif // ARCH_X86_PROCESSOR_HPP
