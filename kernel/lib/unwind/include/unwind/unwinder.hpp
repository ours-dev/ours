/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_UNWINDER_HPP
#define UNWIND_UNWINDER_HPP 1

#include <cstddef>
#include <cstdint>

#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>
#include <unwind/registers.hpp>

namespace unwind {

/// Why a step stopped. Every value other than `Ok` means the current
/// register state is the last trustworthy frame.
enum class StepResult : std::uint8_t {
    Ok,
    /// No FDE covers the current PC — typically a leaf frame or asm.
    NoFde,
    /// Malformed CFI or a rule this unwinder cannot evaluate.
    BadCfi,
    /// The frame data yields an implausible transition (unaligned SP,
    /// non-monotonic stack, null PC, unreadable memory).
    BadFrame,
};

/// Asynchronous memory access seam. Stack-slot reads go through it so the
/// unwinder never dereferences an address it cannot vet; kernel callers
/// bound reads to the interrupted thread's stack, tests back it with a map.
struct MemoryReader final {
    void *context;
    /// Reads 8 bytes at `address` into `*out`; returns false when unmapped.
    auto (*read)(void *context, std::uint64_t address, std::uint64_t *out) -> bool;
};

/// CFI-driven frame stepper over a set of CFI sections plus their index.
class Unwinder final {
  public:
    /// Evaluation safety bound: CFI streams are linear, so a cap only guards
    /// against corrupt data looping forever.
    static constexpr std::uint32_t MAX_CFI_OPS = 4096;

    Unwinder(Section const *sections, std::size_t section_count, FdeIndex const *index) noexcept
        : m_sections(sections), m_section_count(section_count), m_index(index) {}

    /// Advance `regs` from the current frame to its caller. On anything but
    /// `StepResult::Ok` the registers are left untouched.
    auto step(Registers *regs, MemoryReader const &memory) const -> StepResult;

  private:
    struct SectionRef final {
        Section const *section;
        FdeIndex::Entry const *entry;
    };

    [[nodiscard]]
    auto locate(std::uint64_t pc, SectionRef *out) const noexcept -> bool;

    Section const *m_sections;
    std::size_t m_section_count;
    FdeIndex const *m_index;
};

} // namespace unwind

#endif // #ifndef UNWIND_UNWINDER_HPP
