/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_KERNEL_UNWINDER_HPP
#define SAMPLER_KERNEL_UNWINDER_HPP 1

#include <sampler/format.hpp>

#include <span>

#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>
#include <unwind/unwinder.hpp>

namespace sampler {
/// The kernel code a sampling tick interrupted: its registers, from the
/// interrupt frame, and the stack it was running on.
struct KernelFrame final {
    u64 pc;
    u64 sp;
    u64 bp;
    /// The stack the interrupted code was running on, [stack_base,
    /// stack_top): the unwind reads only between sp and its top. Both 0 when
    /// unknown, and then nothing above the pc is read.
    u64 stack_base;
    u64 stack_top;
};

/// Where a kernel stack begins: the return address of a call an entry stub
/// makes into C++, and how far below the top of the thread's stack the stub
/// makes it. The unwinder ends a walk at a frame with no unwind information,
/// and only these frames are where a stack really began; anywhere else such
/// a frame is code the unwinder cannot step through, with the rest of the
/// stack below it. Both must match: the same stub can also be entered from
/// kernel code deeper in the stack, where its frame begins nothing.
struct StackRoot final {
    u64 return_address;
    u64 depth;
};

/// What one unwind found: how many addresses it wrote, and Truncated when
/// the stack goes on past them, or None when they are the whole stack.
struct Unwound final {
    usize depth;
    RecordFlags flags;
};

/// Turns an interrupted kernel frame into its call stack, by the DWARF call
/// frame information of the kernel's `.eh_frame`. It allocates nothing and
/// does bounded work, so it runs in the sampling timer's interrupt.
///
/// What it reads is handed in: the section's bytes, an index of its FDEs
/// sorted by start address, and the stack roots, all kept alive by the
/// caller. Where those come from -- the linker's symbols for the section,
/// the architecture's entry stubs -- is the kernel's business, not this
/// class's, which is why it builds and is tested on the host.
class KernelUnwinder final {
  public:
    struct Tables final {
        /// The kernel's `.eh_frame`; an empty section leaves every stack at
        /// its pc, marked Truncated.
        unwind::Section eh_section{};
        /// Ascending-`pc_begin`-sorted FDE entries (`unwind::FdeIndex::init`'s
        /// storage contract); must outlive this.
        unwind::FdeIndex::Entry const *fde_entries = nullptr;
        usize fde_entry_count = 0;
        /// Where kernel stacks begin; must outlive this. Without any, every
        /// stack that ends at a frame with no unwind information is marked a
        /// prefix.
        StackRoot const *stack_roots = nullptr;
        usize stack_root_count = 0;
    };

    /// Wires up |tables|.
    auto init(Tables const &tables) -> void;

    /// Writes |frame|'s pc and then each caller's return address into
    /// |ips|, at most ips.size() of them; |ips| holds at least one.
    ///
    /// Every read goes through a StackWindow from the interrupted sp to the
    /// top of |frame|'s stack, so a corrupt frame cannot become a read past
    /// it. A frame whose sp is not on that stack keeps its pc alone and is
    /// Truncated. So is a walk that stops at a frame it cannot step through
    /// other than a stack root, or that fills |ips|.
    [[nodiscard]]
    auto unwind(KernelFrame const &frame, std::span<u64> ips) const -> Unwound;

  private:
    /// Whether |regs|, a frame with no unwind information, is one of the
    /// roots: its pc a root's return address, at that root's depth below the
    /// top of |frame|'s stack.
    [[nodiscard]]
    auto is_stack_root(unwind::Registers const &regs, KernelFrame const &frame) const -> bool;

    unwind::Section m_eh_section{};
    unwind::FdeIndex m_fde_index;
    StackRoot const *m_stack_roots = nullptr;
    usize m_stack_root_count = 0;
};
} // namespace sampler

#endif // #ifndef SAMPLER_KERNEL_UNWINDER_HPP
