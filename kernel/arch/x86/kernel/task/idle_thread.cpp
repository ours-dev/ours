#include <ours/arch/idle_thread.hpp>

namespace ours {
/// Halt until the next interrupt, enabling interrupts as part of the halt.
///
/// `sti` takes effect after the instruction that follows it, so `sti; hlt`
/// cannot be split by an interrupt: the CPU either halts with interrupts
/// enabled or is woken by the very interrupt that arrived. The caller decides
/// there is nothing to run with interrupts disabled and relies on this to
/// close the window between that decision and the halt.
///
/// Interrupts are left enabled on return; the caller's interrupt-state guard
/// restores whatever it had.
auto ArchIdleThread::idle() -> void {
    asm volatile("sti; hlt" ::: "memory");
}
} // namespace ours
