// SPDX-License-Identifier: MIT

#include <cstdlib>

#include <arch/x86/processor.hpp>

auto main() -> int {
    arch::X86CpuInfo cpu;
    cpu.init();

    // Exercise both a populated and an absent/set-independent feature query.
    // The result intentionally depends on the executing CPU.
    auto const pcid = cpu.feature().has_feature(arch::CpuFeatureType::Pcid);
    auto const la57 = cpu.feature().has_feature(arch::CpuFeatureType::La57);
    if (pcid && !cpu.feature().has_feature(arch::CpuFeatureType::Pcid)) {
        std::abort();
    }
    if (la57 && !cpu.feature().has_feature(arch::CpuFeatureType::La57)) {
        std::abort();
    }
    return 0;
}
