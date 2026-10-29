/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/panic.hpp>
#include <ours/phys/arch_handoff.hpp>
#include <ours/phys/aspace.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>

namespace ours::phys {
[[noreturn]]
auto arch_handoff_to_kernel(VirtAddr const entry, PhysAddr const pgd, Handoff *const handoff)
    -> void {
    // The kernel's root is the aspace physload built and physboot added the
    // kernel to, so the switch runs in place, from physboot's own text: that
    // is bootmem memory, inside the RAM physload identity-mapped into this
    // very table, so the instructions after the satp write are mapped under
    // the new root as they were under the old. Any other root would have to
    // be shown to map them first, which is what this check stands for.
    auto *const aspace = global_aspace();
    ASSERT(pgd == aspace->pgd(), "riscv hands off on phys's own root 0x{:X}, not 0x{:X}",
           aspace->pgd(), pgd);

    // Installing the root that is already installed is for its fences.
    // physboot wrote the kernel's image and physmap entries into the live
    // tables after physload installed them, and a hart may keep a translation
    // it looked up as invalid until an sfence.vma: without one, the jump to
    // the kernel's high address could still fault.
    aspace->install();

    // No fence.i here: the kernel's instructions were made visible to this
    // hart's fetches when ElfImage::load_at() returned, and the relocations
    // physboot applied after it wrote only the kernel's writable segments.
    // Whatever comes to write the kernel's text after load_at(), such as the
    // code patching physboot does not do yet, has to sync again itself.
    //
    // a0 is the handoff, whose pointer is also its physical address, since
    // phys runs identity-mapped. ra and s0 are cleared so the kernel's first
    // frame is the end of any frame-pointer walk rather than a link into
    // physboot. sp stays on the boot stack, which the kernel replaces; gp and
    // tp keep phys's values, which mean nothing to the kernel.
    register usize const argument asm("a0") = reinterpret_cast<usize>(handoff);
    register VirtAddr const target asm("t0") = entry;
    asm volatile("mv ra, zero\n\t"
                 "mv s0, zero\n\t"
                 "jr %[target]"
                 :
                 : [target] "r"(target), "r"(argument)
                 : "memory");

    panic("The kernel's entry returned");
}

} // namespace ours::phys
