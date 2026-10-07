/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sampler/kernel_unwinder.hpp>
#include <sampler/stack_window.hpp>

#include <unwind/unwinder.hpp>

namespace sampler {
namespace {
    /// Reads one word of the interrupted thread's stack for the unwinder,
    /// refusing anything outside the window (StackWindow::contains()).
    auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (!window->contains(address)) {
            return false;
        }
        *out = *reinterpret_cast<u64 const *>(address);
        return true;
    }
} // namespace

auto KernelUnwinder::init(Tables const &tables) -> void {
    m_eh_section = tables.eh_section;
    m_fde_index.init(tables.fde_entries, tables.fde_entry_count);
    m_stack_roots = tables.stack_roots;
    m_stack_root_count = tables.stack_root_count;
}

auto KernelUnwinder::unwind(KernelFrame const &frame, std::span<u64> const ips) const -> Unwound {
    ips[0] = frame.pc;
    auto result = Unwound{.depth = 1, .flags = RecordFlags::None};

    // Not on the stack the kernel named, or no stack named: nothing above
    // the pc can be read safely, so the pc is kept alone, as a prefix.
    auto window = StackWindow::on_stack(frame.sp, frame.stack_base, frame.stack_top);
    if (window.empty()) {
        result.flags |= RecordFlags::Truncated;
        return result;
    }
    unwind::MemoryReader const reader{&window, &kernel_stack_read};
    unwind::Unwinder const unwinder(&m_eh_section, 1, &m_fde_index);
    unwind::Registers regs{
        .pc = frame.pc,
        .sp = frame.sp,
        .bp = frame.bp,
    };

    while (result.depth < ips.size()) {
        auto const step = unwinder.step(&regs, reader);
        if (step != unwind::StepResult::Ok) {
            // A frame with no caller (a thread's first frame) ends the stack.
            // A frame with no unwind information ends it only where the entry
            // code began it; anywhere else it is assembly the unwinder cannot
            // step through, a user copy or an exception taken in kernel mode,
            // and the stack goes on below it. Any other failure leaves a
            // prefix too.
            bool const complete = step == unwind::StepResult::End ||
                                  (step == unwind::StepResult::NoFde && is_stack_root(regs, frame));
            if (!complete) {
                result.flags |= RecordFlags::Truncated;
            }
            return result;
        }
        ips[result.depth] = regs.pc;
        ++result.depth;
    }
    result.flags |= RecordFlags::Truncated;
    return result;
}

auto KernelUnwinder::is_stack_root(unwind::Registers const &regs, KernelFrame const &frame) const
    -> bool {
    for (usize i = 0; i < m_stack_root_count; ++i) {
        auto const &root = m_stack_roots[i];
        if (regs.pc == root.return_address && frame.stack_top - regs.sp == root.depth) {
            return true;
        }
    }
    return false;
}
} // namespace sampler
