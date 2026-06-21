/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_FAULT_HPP
#define OURS_MEM_FAULT_HPP 1

#include <ours/status.hpp>
#include <ours/mem/types.hpp>
#include <ours/mem/vm_fault.hpp>

namespace ours::mem {
    /// The common handling logic providing to arch-developer.
    ///
    /// @p addr is the virtual address that fault happened at.
    /// @p cause explanates what lead to this fault.
    NO_MANGLE
    auto handle_vm_fault(VirtAddr addr, VmfCause cause) -> Status;

} // namespace ours::mem

#endif // #ifndef OURS_MEM_FAULT_HPP