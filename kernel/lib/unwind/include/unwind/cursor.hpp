/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_CURSOR_HPP
#define UNWIND_CURSOR_HPP 1

#include <cstddef>
#include <cstdint>

namespace unwind {

/// Bounds-checked read cursor over a CFI section (`.eh_frame` / `.debug_frame`).
///
/// Every read either succeeds and advances the cursor or poisons it; poisoned
/// reads keep yielding zero without further state damage, so a caller may
/// issue a whole sequence of reads and inspect `ok()` once per phase. All
/// reads are bounded by the section extent the cursor was constructed with.
///
/// Poisoning drops the data pointer: a cursor with no data is the one state
/// that cannot read, whether it was built over none or failed since, so
/// `ok()` is that pointer's presence rather than a flag kept beside it.
class Cursor final {
  public:
    Cursor(std::uint8_t const *data, std::size_t size) noexcept
        : m_data(data), m_size(size), m_pos(0) {}

    [[nodiscard]]
    auto ok() const noexcept -> bool {
        return m_data != nullptr;
    }

    [[nodiscard]]
    auto offset() const noexcept -> std::size_t {
        return m_pos;
    }

    auto fail() noexcept -> void {
        m_data = nullptr;
    }

    auto u8() noexcept -> std::uint8_t;
    auto u16() noexcept -> std::uint16_t;
    auto u32() noexcept -> std::uint32_t;
    auto u64() noexcept -> std::uint64_t;

    /// DWARF LEB128, unsigned form.
    auto uleb() noexcept -> std::uint64_t;

    /// DWARF LEB128, signed form.
    auto sleb() noexcept -> std::int64_t;

    auto skip(std::size_t count) noexcept -> void;

    auto read_bytes(std::uint8_t *destination, std::size_t count) noexcept -> void;

    /// Read one address stored with a DW_EH_PE pointer encoding. `pc_base` is
    /// the relocation anchor for `DW_EH_PE_PCREL`: the address the encoded
    /// field itself has, in the address space the pointer is used in
    /// (Section::address plus the field's offset), not its offset in the
    /// section. Applications this unwinder
    /// cannot honour (`TEXTREL`/`DATAREL`/`FUNCREL`/`ALIGNED`/`INDIRECT`)
    /// poison the cursor instead of guessing.
    auto encoded_pointer(std::uint8_t encoding, std::uint64_t pc_base) noexcept -> std::uint64_t;

  private:
    auto checked_slice(std::size_t count) noexcept -> std::uint8_t const *;

    std::uint8_t const *m_data;
    std::size_t m_size;
    std::size_t m_pos;
};

} // namespace unwind

#endif // #ifndef UNWIND_CURSOR_HPP
