#include <ours/arch/thread.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/task/thread.hpp>

#include <arch/x86/fsgs.hpp>
#include <arch/x86/msr.hpp>
#include <ustl/mem/align.hpp>
#include <ustl/mem/container_of.hpp>

using namespace arch;

namespace ours::task {
namespace {
    auto swap_gsbase_and_fsbase(usize new_gsbase, usize new_fsbase, usize *old_gs_base,
                                usize *old_fs_base) -> void {
        if (feature_has_fsgsbase) {
            *old_fs_base = read_fsbase<usize>();
        } else {
            *old_fs_base = MsrIo::read<usize>(MsrRegAddr::IA32FsBase);
        }

        if (feature_has_fsgsbase) {
            write_fsbase(new_fsbase);
            *old_gs_base = exchange_gsbase_after_swap(new_gsbase);
        } else {
            MsrIo::write(MsrRegAddr::IA32FsBase, new_fsbase);

            *old_gs_base = MsrIo::read<usize>(MsrRegAddr::IA32KernelGsBase);
            MsrIo::write(MsrRegAddr::IA32KernelGsBase, new_gsbase);
        }
    }
} // namespace

auto X86Thread::switch_thread(Thread *prev, Thread *next) -> void {
    auto &prev_arch = prev->m_arch_thread;
    auto &next_arch = next->m_arch_thread;

    // Set stack pointer which was used at privilege 0.
    x86_set_tss_sp(next->kernel_stack().top());

    swap_gsbase_and_fsbase(next_arch.m_gs_base, next_arch.m_fs_base, &prev_arch.m_gs_base,
                           &prev_arch.m_fs_base);

    // The assembly is handed two stack slots and nothing else. Keeping it
    // ignorant of Thread's layout is deliberate: context.S would otherwise
    // need an offset constant kept in step with the C++ struct by hand.
    x86_switch_context(&prev_arch.m_sp, next_arch.m_sp);
}

auto X86Thread::init(VirtAddr entry_point) -> void {
    struct x64ContextSwitchFrame {
        u64 r15;
        u64 r14;
        u64 r13;
        u64 r12;
        u64 rbp;
        u64 rbx;
        u64 rip;
    };

    auto const thread = Thread::of(this);
    auto const stack_top = thread->kernel_stack().top();

    DEBUG_ASSERT(ustl::mm::is_aligned(stack_top, 16));

    // Before to construct a frame on stack, we must preserve 8 byte size of space
    // for caller's the return address.
    auto frame = reinterpret_cast<x64ContextSwitchFrame *>(stack_top - 8);
    frame--;
    memset(frame, 0, sizeof(*frame));

    frame->rip = entry_point;
    m_sp = reinterpret_cast<VirtAddr>(frame);

    m_fs_base = 0;
    m_gs_base = 0;
}

} // namespace ours::task
