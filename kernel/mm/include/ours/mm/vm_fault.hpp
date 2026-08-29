/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_FAULT_HPP
#define OURS_MM_VM_FAULT_HPP 1

#include <ours/mm/page_request.hpp>

#include <ours/types.hpp>
#include <ours/macro_abi.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mm {
    enum class VmfCause {
        None,
        Write = BIT(0),
        Absent = BIT(1),
        User = BIT(2),

        /// Fault request from software.
        Software = BIT(3),
    };
    USTL_ENABLE_ENUM_BITMASK(VmfCause);

    struct VmFault {
        VirtAddr va;
        VirtAddr num_pages;
        VmfCause cause;
        PageRequest page_request;
    };

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_FAULT_HPP