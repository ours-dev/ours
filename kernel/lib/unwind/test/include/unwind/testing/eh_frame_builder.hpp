/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef UNWIND_TESTING_EH_FRAME_BUILDER_HPP
#define UNWIND_TESTING_EH_FRAME_BUILDER_HPP 1

#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>

#include <vector>

#include <cstddef>
#include <cstdint>

/// A minimal .eh_frame emitter shaped like x86-64 clang output, for tests
/// that need call frame information without a compiler: the unwinder's own,
/// and those of anything that unwinds with it. Opcode constants are spelled
/// here: the evaluator keeps its table private.
namespace unwind::testing {
inline constexpr std::uint8_t CFA_NOP = 0x00;
inline constexpr std::uint8_t CFA_UNDEFINED = 0x07; // uleb register
inline constexpr std::uint8_t CFA_DEF_CFA = 0x0C;
inline constexpr std::uint8_t CFA_DEF_CFA_REGISTER = 0x0D;
inline constexpr std::uint8_t CFA_DEF_CFA_OFFSET = 0x0E;
inline constexpr std::uint8_t CFA_DEF_CFA_SF = 0x12;
inline constexpr std::uint8_t CFA_OFFSET = 0x80;  // + reg, uleb factored offset
inline constexpr std::uint8_t CFA_RESTORE = 0xC0; // + reg
inline constexpr std::uint8_t CFA_ADVANCE = 0x40; // + byte count
inline constexpr std::uint8_t CFA_REMEMBER_STATE = 0x0A;
inline constexpr std::uint8_t CFA_RESTORE_STATE = 0x0B;
inline constexpr std::uint8_t ENC_PCREL_SDATA4 = 0x1B; // DW_EH_PE_pcrel | DW_EH_PE_sdata4

/// The DWARF register number of the return address on x86-64, the CIE's
/// return address column.
inline constexpr std::uint8_t RETURN_ADDRESS_COLUMN = 16;

class SectionBuilder final {
  public:
    auto u8(std::uint8_t value) -> SectionBuilder & {
        m_bytes.push_back(value);
        return *this;
    }

    auto u32(std::uint32_t value) -> SectionBuilder & {
        for (std::size_t i = 0; i < 4; ++i) {
            m_bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
        return *this;
    }

    auto u64(std::uint64_t value) -> SectionBuilder & {
        for (std::size_t i = 0; i < 8; ++i) {
            m_bytes.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
        return *this;
    }

    auto uleb(std::uint64_t value) -> SectionBuilder & {
        while (true) {
            std::uint8_t const byte = value & 0x7F;
            value >>= 7;
            if (value != 0) {
                m_bytes.push_back(byte | 0x80);
                continue;
            }
            m_bytes.push_back(byte);
            return *this;
        }
    }

    auto sleb(std::int64_t value) -> SectionBuilder & {
        while (true) {
            std::uint8_t const byte = static_cast<std::uint8_t>(value & 0x7F);
            value >>= 7;
            bool const more = (value != 0 && value != -1) ||
                              ((value == -1) && ((byte & 0x40) == 0)) ||
                              ((value == 0) && ((byte & 0x40) != 0));
            if (more) {
                m_bytes.push_back(byte | 0x80);
                continue;
            }
            m_bytes.push_back(byte);
            return *this;
        }
    }

    auto sdata4(std::int32_t value) -> SectionBuilder & {
        return u32(static_cast<std::uint32_t>(value));
    }

    auto patch_u32(std::size_t offset, std::uint32_t value) -> void {
        for (std::size_t i = 0; i < 4; ++i) {
            m_bytes[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
    }

    auto seal(std::size_t start) -> void {
        patch_u32(start, static_cast<std::uint32_t>(m_bytes.size() - start - 4));
    }

    [[nodiscard]]
    auto size() const -> std::size_t {
        return m_bytes.size();
    }

    [[nodiscard]]
    auto data() const -> std::uint8_t const * {
        return m_bytes.data();
    }

  private:
    std::vector<std::uint8_t> m_bytes;
};

class FakeEhFrame final {
  public:
    // CIE with the canonical initial program appended: def_cfa(rsp, 8) and
    // return-address rule offset(1).
    auto emit_cie() -> void {
        std::size_t const start = m_out.size();
        m_out
            .u32(0) // length
            .u32(0) // CIE id
            .u8(1)  // version
            .u8('z')
            .u8('R')
            .u8(0)
            .uleb(1)  // code alignment
            .sleb(-8) // data alignment
            .uleb(RETURN_ADDRESS_COLUMN)
            .uleb(1) // augmentation data length
            .u8(ENC_PCREL_SDATA4)
            .u8(CFA_DEF_CFA)
            .uleb(7)
            .uleb(8)
            .u8(CFA_OFFSET | RETURN_ADDRESS_COLUMN)
            .uleb(1);
        m_out.seal(start);
        m_cie_offset = start;
    }

    auto emit_fde(std::uint64_t pc, std::uint64_t range, std::uint8_t const *ins, std::size_t count)
        -> void {
        std::size_t const start = m_out.size();
        m_out.u32(0);
        m_out.u32(static_cast<std::uint32_t>(start + 4 - m_cie_offset));
        std::size_t const pc_field = m_out.size();
        m_out.sdata4(static_cast<std::int32_t>(pc - pc_field));
        m_out.sdata4(static_cast<std::int32_t>(range));
        m_out.uleb(0); // FDE augmentation-data length (CIE uses "z")
        for (std::size_t i = 0; i < count; ++i) {
            m_out.u8(ins[i]);
        }
        m_out.seal(start);
    }

    /// The buffer pretends to be loaded at address 0, so that a PC-relative
    /// field at offset N anchors at N and the PCs stay small numbers.
    [[nodiscard]]
    auto section() const -> unwind::Section {
        return unwind::Section{m_out.data(), m_out.size(), unwind::Section::Kind::EhFrame, 0};
    }

  private:
    SectionBuilder m_out;
    std::uint64_t m_cie_offset = 0;
};

/// Builds the FDE index of |section| into |entries|, as the kernel does at
/// boot: whether the section parsed, and how many FDEs it had in |count|.
inline auto build_fde_entries(unwind::Section const &section,
                              std::vector<unwind::FdeIndex::Entry> *const entries,
                              std::size_t *const count) -> bool {
    auto const sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
        static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
        return true;
    };
    return unwind::FdeIndex::build(section, 0, entries, sink, count);
}
} // namespace unwind::testing

#endif // #ifndef UNWIND_TESTING_EH_FRAME_BUILDER_HPP
