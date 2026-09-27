/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_FAULT_HPP
#define OURS_MM_VM_FAULT_HPP 1

#include <ours/macro_abi.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
enum class VmfCause {
    None,
    Write = BIT(0),
    Absent = BIT(1),
    User = BIT(2),

    /// Fault request from software.
    Software = BIT(3),

    /// The access was an instruction fetch.
    ///
    /// Without it an instruction fetch looks like a read to the mapping, and a
    /// read is allowed on a page that is present but not executable. The
    /// mapping then answered a fetch from such a page (a user stack, which is
    /// NX) as a spurious fault, the CPU retried the same fetch, and it faulted
    /// again for ever instead of killing the process. VmMapping::page_fault()
    /// refuses this cause on a region without MmuFlags::Executable.
    ///
    /// Zircon: VMM_PF_FLAG_INSTRUCTION.
    Instruction = BIT(4),
};
USTL_ENABLE_ENUM_BITMASK(VmfCause);

/// One page fault, as it travels from the architecture's fault entry down to
/// the mapping that has to resolve it.
///
/// Built on the faulting thread's own stack by VmAspace::page_fault_internal()
/// and handed down by pointer. VmMapping::page_fault() used to be given a literal
/// `0` in its place.
struct VmFault {
    /// The faulting address exactly as the hardware reported it, not page
    /// aligned -- x86 takes it straight out of CR2. VmMapping::page_fault() aligns
    /// it down itself, so the unaligned value stays available to anything that
    /// wants to know which byte was touched.
    VirtAddr va;

    /// How many pages the resolver may serve in one go, the faulting one
    /// included. A ceiling, not a demand: fault() serves one page today, and
    /// mapping ahead needs the VmCowPages cursor to hand back runs first.
    usize num_pages;

    VmfCause cause;

    /// The waiting token, held by value so that it lives on the faulting
    /// thread's stack for as long as the retry loop around it runs.
    ///
    /// The fault path must not allocate: an allocation here can recurse into
    /// reclaim, which can fault again. That is the reason the whole
    /// require_page() chain threads a MultiPageRequest * down from the caller
    /// instead of making one where it is finally needed, and the reason this
    /// is a member rather than something fault() creates.
    ///
    /// MultiPageRequest rather than a bare PageRequest because that is what
    /// the chain below takes -- it bundles the read request with the anonymous
    /// one, and each layer picks out the slot it knows how to fill.
    MultiPageRequest page_request;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_FAULT_HPP