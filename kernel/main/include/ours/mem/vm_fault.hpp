/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_FAULT_HPP
#define OURS_MEM_VM_FAULT_HPP 1

#include <ours/mem/page-request.hpp>

#include <ours/types.hpp>
#include <ours/macro_abi.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::mem {
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

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_FAULT_HPP