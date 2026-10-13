/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_TLB_HPP
#define ARCH_TLB_HPP 1

namespace arch {

/// Flushes every TLB entry on the current CPU.
auto tlb_invalidate_all() -> void;

/// Flushes every TLB entry on the current CPU, global ones included: what a
/// CPU coming back from being offline needs, since it took no shootdown
/// while it was away, the kernel's own among them.
auto tlb_invalidate_all_global() -> void;

} // namespace arch

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately.
#if defined(ARCH_X86)
#include <arch/x86/tlb.hpp>
#endif

#endif // ARCH_TLB_HPP
