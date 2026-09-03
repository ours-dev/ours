/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_INIT_STAGE_HPP
#define OURS_MM_INIT_STAGE_HPP 1

#include <ours/compiler.hpp>

/// How far memory-management bring-up has got.
///
/// Split out of <ours/mm/init.hpp> because that header pulls in
/// <ours/phys/handoff.hpp> for the handoff declarations, and the stage alone
/// has no business dragging the phys headers into everything that asks. pmm.hpp
/// derives pmm_enabled() from this, and pmm.hpp is included nearly everywhere.
namespace ours::mm {
enum class InitStage {
    BootStrap,
    Pmm,
    Vmm,
    All,
};

extern InitStage INIT_STAGE;

FORCE_INLINE inline auto get_init_stage() -> InitStage {
    return INIT_STAGE;
}

FORCE_INLINE inline auto set_init_stage(InitStage stage) -> void {
    INIT_STAGE = stage;
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_INIT_STAGE_HPP
