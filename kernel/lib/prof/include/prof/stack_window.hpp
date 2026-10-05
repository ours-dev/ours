/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_STACK_WINDOW_HPP
#define PROF_STACK_WINDOW_HPP 1

namespace prof {

/// The part of a stack the kernel unwinder may read: from the interrupted
/// stack pointer up to the top of the stack that pointer is on.
///
/// Everything the unwinder reads is an address it computed from a frame it
/// cannot trust -- a frame pointer, a CFA -- and it reads it inside the
/// sampling interrupt, where a fault cannot be taken. So every read goes
/// through contains() first. The window ends at the stack's own top rather
/// than at some distance above the stack pointer: a kernel stack is a
/// mapping of its own, and what lies past its top need not be mapped at all.
class StackWindow final {
  public:
    /// The window [sp, top) of the stack [base, top). Empty when sp is not on
    /// that stack -- the interrupted code was running on some other stack, or
    /// the caller does not know the bounds (base == top == 0) -- in which case
    /// nothing may be read.
    [[nodiscard]]
    static constexpr auto on_stack(u64 const sp, u64 const base, u64 const top) -> StackWindow {
        if (base >= top || sp < base || sp >= top) {
            return StackWindow{};
        }
        return StackWindow{sp, top};
    }

    /// Whether the 8-byte word at |address| lies wholly inside the window and
    /// is aligned.
    ///
    /// Written as `address > hi - 8`, never `address + 8 > hi`: |address| is
    /// the arbitrary input here, and near the top of the address space
    /// `address + 8` wraps to a small number that would pass. `hi - 8` cannot
    /// underflow, because the size check before it returns first for a window
    /// narrower than a word.
    ///
    /// Alignment is checked too. x86 tolerates an unaligned load, but the
    /// library is built for arm and riscv as well, where the same corrupt
    /// pointer would fault inside the interrupt instead of being refused.
    [[nodiscard]]
    constexpr auto contains(u64 const address) const -> bool {
        if (m_hi - m_lo < sizeof(u64)) {
            return false;
        }
        if (address < m_lo || address > m_hi - sizeof(u64)) {
            return false;
        }
        return (address % alignof(u64)) == 0;
    }

    [[nodiscard]]
    constexpr auto empty() const -> bool {
        return m_lo == m_hi;
    }

  private:
    constexpr StackWindow() = default;
    constexpr StackWindow(u64 const lo, u64 const hi) : m_lo(lo), m_hi(hi) {}

    u64 m_lo = 0;
    u64 m_hi = 0;
};

} // namespace prof

#endif // #ifndef PROF_STACK_WINDOW_HPP
