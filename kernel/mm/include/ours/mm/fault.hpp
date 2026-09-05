/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_FAULT_HPP
#define OURS_MM_FAULT_HPP 1

#include <ours/mm/types.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/status.hpp>

namespace ours::mm {
/// The common handling logic providing to arch-developer.
///
/// @p addr is the virtual address that fault happened at.
/// @p cause explanates what lead to this fault.
extern "C" auto handle_vm_fault(VirtAddr addr, VmfCause cause) -> Status;

} // namespace ours::mm

#endif // #ifndef OURS_MM_FAULT_HPP
