/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef HEAP_VMALLOC_HPP
#define HEAP_VMALLOC_HPP 1

#include <ours/arch/aspace_layout.hpp>
#include <ours/mm/alloc_flags.hpp>
#include <ours/types.hpp>

namespace heap {
using ours::VirtAddr;
using ours::mm::AllocFlags;

/// The window vmalloc hands addresses out of, as a pair of constants rather
/// than as something the allocator is told at run time: is_vmalloc_addr()
/// below is on the path of every `operator delete`, and a compile-time range
/// makes it two instructions.
///
/// The range is half-open throughout, `[VMALLOC_BASE, VMALLOC_BASE + SIZE)`,
/// and no constant names the last valid address: an inclusive end tested
/// with `<`, as is_vmalloc_addr() tests, would leave the window's last byte
/// outside it.
inline constexpr auto VMALLOC_BASE = ours::ARCH_VMALLOC_VIRT_BASE;
inline constexpr auto VMALLOC_SIZE = ours::ARCH_VMALLOC_SIZE;

/// Whether |pointer| came from vmalloc().
///
/// This is what lets `operator delete` route a bare pointer, and it works
/// because the two allocators return addresses from disjoint regions:
/// kmalloc's come from the physmap, vmalloc's from the window above. There is
/// no header on either allocation and no size to consult, so the address is
/// the only thing there is to ask.
///
/// The subtraction form answers both ends at once and cannot wrap, the same
/// shape ours::mm::is_kernel_address() uses.
[[nodiscard]]
FORCE_INLINE
constexpr auto is_vmalloc_addr(void const *const pointer) -> bool {
    return VirtAddr{reinterpret_cast<usize>(pointer)} - VMALLOC_BASE < VMALLOC_SIZE;
}

/// Brings the window's bookkeeping up. Must run after the kernel address
/// space exists and before the first vmalloc(); //kernel/mm's init_heap()
/// calls it, which is the point in init_vmm() named for exactly this.
auto init_vmalloc() -> void;

/// |size| bytes of virtually contiguous kernel memory, page aligned and
/// rounded up to a whole number of pages.
///
/// Returns nullptr rather than a Status, matching kmalloc() beside it: every
/// caller has to test the pointer anyway, and `operator new` has nowhere to
/// put a status.
///
/// The memory is NOT zeroed unless |alloc_flags| carries AllocFlags::Zero. Pass it when the
/// contents matter; the pages come straight from the buddy allocator and hold
/// whatever the last owner left.
///
/// May block: it takes a mutex and allocates frames, so it must not be called
/// from an interrupt handler or with a spin lock held. This is a stricter
/// contract than kmalloc()'s, and `operator new` inherits it above the size
/// threshold -- see the note in new.cpp.
[[nodiscard]]
auto vmalloc(usize size, AllocFlags alloc_flags) -> void *;

/// Returns a pointer vmalloc() handed out. nullptr is accepted and ignored,
/// as free() has always been allowed to be.
auto vfree(void *pointer) -> void;

/// How many ranges are currently handed out. The boot self-check compares it
/// before and after a round of allocations to prove every mapping was retired.
[[nodiscard]]
auto vmalloc_live_ranges() -> usize;

/// Runs a round of allocations and frees and reports the result to the log.
/// Called once from init_heap(), immediately after init_vmalloc().
auto vmalloc_self_test() -> void;

} // namespace heap

#endif // #ifndef HEAP_VMALLOC_HPP
