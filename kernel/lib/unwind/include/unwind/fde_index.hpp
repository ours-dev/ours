/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_FDE_INDEX_HPP
#define UNWIND_FDE_INDEX_HPP 1

#include <cstddef>
#include <cstdint>

#include <unwind/cfi.hpp>

namespace unwind {

/// PC-range lookup over the FDEs of one CFI section.
///
/// The index itself owns no storage: `build` streams entries to a
/// caller-provided sink, the caller sorts them into ascending `pc_begin`
/// order with `finalize`, and lookups binary-search the array. This keeps
/// the core allocation-free — the kernel backs the array with `ktl::Vec`
/// (`kmalloc`), host tests with `std::vector`.
class FdeIndex final {
  public:
    struct Entry final {
        std::uint64_t pc_begin;
        std::uint64_t pc_end;
        std::uint64_t fde_offset;
        std::uint32_t section;
    };

    /// Sink invoked once per FDE in scan order. Returning false aborts the
    /// build; the number of produced entries is reported through `*count`.
    using Sink = auto (*)(void *context, Entry const &entry) -> bool;

    /// Scan `section` and emit one `Entry` per FDE, tagging them with
    /// `section_id`. Returns false when the section is malformed. `*count`
    /// receives the number of emitted entries.
    static auto build(Section const &section, std::uint32_t section_id, void *context, Sink sink,
                      std::size_t *count) -> bool;

    /// Take ownership of a caller-sorted entry array. The array must outlive
    /// the index and be sorted by ascending `pc_begin` (overlapping ranges
    /// are resolved by first match, which favours exact-range entries).
    auto init(Entry const *entries, std::size_t count) noexcept -> void {
        m_entries = entries;
        m_count = count;
    }

    /// Locate the FDE covering `pc`, or nullptr when no entry does.
    [[nodiscard]]
    auto find(std::uint64_t pc) const noexcept -> Entry const *;

  private:
    Entry const *m_entries = nullptr;
    std::size_t m_count = 0;
};

} // namespace unwind

#endif // #ifndef UNWIND_FDE_INDEX_HPP
