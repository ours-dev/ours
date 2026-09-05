#include <arch/x86/processor/feature.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/init.hpp>

namespace ours {
auto x86_has_feature(CpuFeatureType type) -> bool {
    // CPUID is per logical CPU.  Querying it here keeps feature-dependent
    // register programming correct on heterogeneous systems until the
    // CPU-local storage subsystem is available during later boot.
    arch::X86CpuFeature features;
    features.init();
    return features.has_feature(type);
}

bool FEATURE_HAS_FSGSBASE = false;

INIT_CODE
auto x86_init_feature_percpu(CpuNum cpunum) -> void {
    if (cpunum == 0) {
        FEATURE_HAS_FSGSBASE = x86_has_feature(CpuFeatureType::FsGsBase);
    }
}

INIT_CODE
auto x86_init_feature_early() -> void {
    x86_init_feature_percpu(0);
}

} // namespace ours
