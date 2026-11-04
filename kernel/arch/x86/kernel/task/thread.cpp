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
    auto swap_gsbase_and_fsbase(VirtAddr new_gsbase, VirtAddr new_fsbase, VirtAddr *old_gs_base,
                                VirtAddr *old_fs_base) -> void {
        if (FEATURE_HAS_FSGSBASE) {
            *old_fs_base = VirtAddr{read_fsbase<usize>()};
        } else {
            *old_fs_base = VirtAddr{MsrIo::read<usize>(MsrRegAddr::IA32FsBase)};
        }

        if (FEATURE_HAS_FSGSBASE) {
            write_fsbase(new_fsbase);
            *old_gs_base = exchange_gsbase_after_swap(new_gsbase);
        } else {
            MsrIo::write(MsrRegAddr::IA32FsBase, std::to_underlying(new_fsbase));

            *old_gs_base = VirtAddr{MsrIo::read<usize>(MsrRegAddr::IA32KernelGsBase)};
            MsrIo::write(MsrRegAddr::IA32KernelGsBase, std::to_underlying(new_gsbase));
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

auto X86Thread::current_user_copy() -> UserCopyState * {
    return &Thread::Current::get()->m_arch_thread.m_user_copy;
}

auto X86Thread::init(VirtAddr entry_point) -> void {
    struct X64ContextSwitchFrame {
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

    DEBUG_ASSERT(ustl::is_aligned(stack_top, 16));

    // Before to construct a frame on stack, we must preserve 8 byte size of space
    // for caller's the return address.
    auto frame = reinterpret_cast<X64ContextSwitchFrame *>(stack_top - 8);
    frame--;
    memset(frame, 0, sizeof(*frame));

    frame->rip = std::to_underlying(entry_point);
    m_sp = VirtAddr{reinterpret_cast<usize>(frame)};

    m_fs_base = VirtAddr{};
    m_gs_base = VirtAddr{};
}

} // namespace ours::task
