/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>

#include <initializer_list>
#include <string_view>
#include <vector>

#include <cstdint>

#include <gtest/gtest.h>

namespace {

// Little-endian CFI section builder for hand-crafted records whose exact
// byte layout the parser contract is checked against.
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

    auto raw(std::uint8_t const *bytes, std::size_t count) -> SectionBuilder & {
        for (std::size_t i = 0; i < count; ++i) {
            m_bytes.push_back(bytes[i]);
        }
        return *this;
    }

    auto patch_u32(std::size_t offset, std::uint32_t value) -> void {
        for (std::size_t i = 0; i < 4; ++i) {
            m_bytes[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
    }

    // Seal a record: patch its 32-bit length so it covers everything pushed
    // since `start` (which points at the length field itself).
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

// A canonical .eh_frame CIE: augmentation "zR", 1-byte code alignment,
// data alignment -8, return address column 16, pcrel|sdata4 FDE encoding.
class EhFrameBuilder final {
  public:
    explicit EhFrameBuilder(SectionBuilder &out) : m_out(out) {}

    auto emit_cie(std::uint8_t fde_encoding) -> void {
        std::size_t const start = m_out.size();
        m_out
            .u32(0) // length, patched by seal
            .u32(0) // CIE id
            .u8(1)  // version
            .u8('z')
            .u8('R')
            .u8(0)
            .uleb(1)  // code_align
            .sleb(-8) // data_align
            .uleb(16) // ra_column
            .uleb(1)  // augmentation data length
            .u8(fde_encoding);
        m_out.seal(start);
        m_cie_offset = start;
    }

    /// A CIE with |augmentation| and its augmentation data as given, the
    /// rest as emit_cie()'s; FDEs emitted after it reference it.
    auto emit_cie_with(std::string_view augmentation, std::initializer_list<std::uint8_t> data)
        -> void {
        std::size_t const start = m_out.size();
        m_out.u32(0).u32(0).u8(1);
        for (char const letter: augmentation) {
            m_out.u8(static_cast<std::uint8_t>(letter));
        }
        m_out.u8(0).uleb(1).sleb(-8).uleb(16).uleb(data.size());
        for (std::uint8_t const byte: data) {
            m_out.u8(byte);
        }
        m_out.seal(start);
        m_cie_offset = start;
    }

    /// The CIE the next FDEs reference: one emitted earlier, by its offset.
    auto use_cie(std::uint64_t offset) -> void {
        m_cie_offset = offset;
    }

    [[nodiscard]]
    auto cie_offset() const -> std::uint64_t {
        return m_cie_offset;
    }

    auto emit_fde(std::uint64_t pc, std::uint64_t range, std::uint8_t const *ins,
                  std::size_t ins_count) -> void {
        std::size_t const start = m_out.size();
        m_out.u32(0); // length, patched by seal
        std::size_t const cie_ref_field = m_out.size();
        m_out.u32(static_cast<std::uint32_t>(cie_ref_field - m_cie_offset));
        // pc_begin: pcrel sdata4, anchored at the datum itself, which sits
        // at m_address plus its offset in the buffer.
        std::size_t const pc_begin_field = m_out.size();
        m_out.sdata4(static_cast<std::int32_t>(pc - (m_address + pc_begin_field)));
        m_out.sdata4(static_cast<std::int32_t>(range));
        m_out.uleb(0); // FDE augmentation-data length (CIE uses "z")
        m_out.raw(ins, ins_count);
        m_out.seal(start);
    }

    [[nodiscard]]
    auto section() const -> unwind::Section {
        return unwind::Section{m_out.data(), m_out.size(), unwind::Section::Kind::EhFrame,
                               m_address};
    }

    /// Where the section pretends to be loaded: 0 unless a test says
    /// otherwise, so that its PCs are small numbers.
    auto set_address(std::uint64_t address) -> void {
        m_address = address;
    }

  private:
    SectionBuilder &m_out;
    std::uint64_t m_cie_offset = 0;
    std::uint64_t m_address = 0;
};

TEST(IdentifyEntry, ClassifiesCieFdeAndTerminator) {
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    std::uint64_t const fde_offset = out.size();
    eh.emit_fde(0x1000, 0x80, FDE_INS, sizeof(FDE_INS));
    out.u32(0); // terminator

    unwind::Section const section = eh.section();

    unwind::EntryKind kind = unwind::EntryKind::Terminator;
    std::uint64_t length = 0;
    ASSERT_TRUE(unwind::identify_entry(section, 0, &kind, &length));
    EXPECT_EQ(kind, unwind::EntryKind::Cie);
    EXPECT_EQ(length, fde_offset);

    ASSERT_TRUE(unwind::identify_entry(section, fde_offset, &kind, &length));
    EXPECT_EQ(kind, unwind::EntryKind::Fde);
    EXPECT_EQ(length, out.size() - fde_offset - 4);

    std::uint64_t const terminator_offset = out.size() - 4;
    ASSERT_TRUE(unwind::identify_entry(section, terminator_offset, &kind, &length));
    EXPECT_EQ(kind, unwind::EntryKind::Terminator);
    EXPECT_EQ(length, 4u);
}

TEST(IdentifyEntry, RejectsDwarf64Escape) {
    SectionBuilder out;
    out.u32(0xFFFF'FFFF).u64(64);
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    unwind::EntryKind kind = unwind::EntryKind::Terminator;
    std::uint64_t length = 0;
    EXPECT_FALSE(unwind::identify_entry(section, 0, &kind, &length));
}

TEST(ParseCie, ExtractsAlignmentFactorsRaColumnAndFdeEncoding) {
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    unwind::Section const section = eh.section();

    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &cie));
    EXPECT_EQ(cie.version, 1);
    EXPECT_EQ(cie.code_align, 1u);
    EXPECT_EQ(cie.data_align, -8);
    EXPECT_EQ(cie.ra_column, 16u);
    EXPECT_EQ(cie.fde_encoding, unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    EXPECT_TRUE(cie.augmentation_present);
    // The initial-instruction program runs to the end of the record.
    EXPECT_EQ(cie.instructions_begin, out.size());
    EXPECT_EQ(cie.instructions_begin, cie.instructions_end);
}

TEST(ParseCie, RejectsUnknownAugmentation) {
    // An unknown letter inside a "z" augmentation is a hard error: the data
    // layout that follows cannot be trusted.
    SectionBuilder out;
    out.u32(0).u32(0).u8(1).u8('z').u8('x').u8(0).uleb(1).sleb(-8).uleb(16).uleb(0);
    out.patch_u32(0, static_cast<std::uint32_t>(out.size() - 4));
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    unwind::Cie cie = {};
    EXPECT_FALSE(unwind::parse_cie(section, 0, &cie));
}

TEST(ParseCie, StepsOverAnIndirectPersonality) {
    // What clang emits for code with a personality: "zPLR", the personality
    // as indirect|pcrel|sdata4. Its value is only skipped, so the indirect
    // bit must not fail the CIE.
    constexpr std::uint8_t PCREL_SDATA4 = unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4;
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie_with("zPLR", {unwind::DW_EH_PE_INDIRECT | PCREL_SDATA4, 0x10, 0x00, 0x00, 0x00,
                              PCREL_SDATA4, PCREL_SDATA4});
    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(eh.section(), 0, &cie));
    EXPECT_EQ(cie.fde_encoding, PCREL_SDATA4);
    EXPECT_FALSE(cie.signal_frame);
}

TEST(ParseCie, AcceptsASignalFrame) {
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie_with("zRS", {unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4});
    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(eh.section(), 0, &cie));
    EXPECT_TRUE(cie.signal_frame);
}

TEST(ParseFde, ResolvesPcrelBeginAndPlainRange) {
    constexpr std::uint64_t FUNCTION_PC = 0x1000;
    constexpr std::uint64_t FUNCTION_RANGE = 0x100;

    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    std::size_t const cie_offset = out.size();
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    eh.emit_fde(FUNCTION_PC, FUNCTION_RANGE, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &cie));

    unwind::Fde fde = {};
    ASSERT_TRUE(unwind::parse_fde(section, cie_offset, cie, &fde));
    EXPECT_EQ(fde.pc_begin, FUNCTION_PC);
    EXPECT_EQ(fde.pc_range, FUNCTION_RANGE);
    EXPECT_EQ(fde.cie_offset, std::uint64_t{0});
    // One NOP instruction inside the record.
    EXPECT_EQ(fde.instructions_end - fde.instructions_begin, 1u);
}

// The kernel's case: .eh_frame read in place, high in the address space. A
// PC-relative pc_begin is relative to where its field is, so the same bytes
// read as if the section sat at 0 name a different, wrong, function start.
// Every test above pretends the section is at 0, where the two readings
// agree, which is how reading the field's offset as its address went
// unnoticed.
TEST(ParseFde, PcrelBeginIsMeasuredFromWhereTheSectionIs) {
    constexpr std::uint64_t SECTION_ADDRESS = 0xffff'ff88'4009'fa08;
    constexpr std::uint64_t FUNCTION_PC = 0xffff'ff88'4003'1ea0;

    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.set_address(SECTION_ADDRESS);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    std::size_t const fde_offset = out.size();
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    eh.emit_fde(FUNCTION_PC, 0xe4, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &cie));
    unwind::Fde fde = {};
    ASSERT_TRUE(unwind::parse_fde(section, fde_offset, cie, &fde));
    EXPECT_EQ(fde.pc_begin, FUNCTION_PC);

    unwind::Section at_zero = section;
    at_zero.address = 0;
    unwind::Fde misread = {};
    ASSERT_TRUE(unwind::parse_fde(at_zero, fde_offset, cie, &misread));
    EXPECT_EQ(misread.pc_begin, FUNCTION_PC - SECTION_ADDRESS);
}

TEST(ParseFde, RejectsCieMismatch) {
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    std::size_t const cie_offset = out.size();
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    eh.emit_fde(0x1000, 0x100, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    unwind::Cie right_cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &right_cie));
    unwind::Cie wrong_cie = right_cie;
    wrong_cie.offset = 1234;

    unwind::Fde fde = {};
    EXPECT_FALSE(unwind::parse_fde(section, cie_offset, wrong_cie, &fde));
    EXPECT_TRUE(unwind::parse_fde(section, cie_offset, right_cie, &fde));
}

namespace {
    auto collecting_sink(void *context, unwind::FdeIndex::Entry const &entry) -> bool {
        static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
        return true;
    }

    auto sorted(std::vector<unwind::FdeIndex::Entry> entries)
        -> std::vector<unwind::FdeIndex::Entry> {
        for (std::size_t i = 1; i < entries.size(); ++i) {
            auto key = entries[i];
            std::size_t j = i;
            while (j > 0 && entries[j - 1].pc_begin > key.pc_begin) {
                entries[j] = entries[j - 1];
                --j;
            }
            entries[j] = key;
        }
        return entries;
    }
} // namespace

TEST(FdeIndex, BuildsWithPcRangesAndFindsCoveredPcs) {
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop

    // Emitted out of PC order on purpose: the index is built from the scan
    // and the caller sorts it.
    eh.emit_fde(0x2000, 0x100, FDE_INS, sizeof(FDE_INS));
    eh.emit_fde(0x1000, 0x80, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &cie));
    EXPECT_EQ(cie.offset, std::uint64_t{0});

    std::vector<unwind::FdeIndex::Entry> entries;
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, &collecting_sink, &count));
    EXPECT_EQ(count, entries.size());
    EXPECT_EQ(count, 2u);

    unwind::FdeIndex index;
    auto const ordered = sorted(std::move(entries));
    index.init(ordered.data(), ordered.size());

    unwind::FdeIndex::Entry const *hit = index.find(0x1042);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->pc_begin, 0x1000u);
    EXPECT_EQ(hit->pc_end, 0x1080u);
    EXPECT_EQ(hit->fde_offset, ordered[0].fde_offset);
    EXPECT_EQ(hit->section, 0u);

    EXPECT_EQ(index.find(0x1080), nullptr); // end is exclusive
    EXPECT_EQ(index.find(0x0FFF), nullptr);
    EXPECT_EQ(index.find(0x2042), ordered.data() + 1);
}

TEST(FdeIndex, FindsPcsOfASectionLoadedHighUp) {
    constexpr std::uint64_t SECTION_ADDRESS = 0xffff'ff88'4009'fa08;
    constexpr std::uint64_t FUNCTION_PC = 0xffff'ff88'4003'1ea0;

    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.set_address(SECTION_ADDRESS);
    eh.emit_cie(unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4);
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    eh.emit_fde(FUNCTION_PC, 0xe4, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    std::vector<unwind::FdeIndex::Entry> entries;
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, &collecting_sink, &count));
    unwind::FdeIndex index;
    index.init(entries.data(), entries.size());

    // A pc inside the function, as an interrupt would capture it.
    auto const *hit = index.find(FUNCTION_PC + 0x88);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->pc_begin, FUNCTION_PC);
}

TEST(FdeIndex, DropsOnlyTheFdesOfACieItDeclines) {
    // One CIE with a letter the parser does not know, between two FDEs of a
    // CIE it reads: only that CIE's FDE is missing from the index.
    constexpr std::uint8_t PCREL_SDATA4 = unwind::DW_EH_PE_PCREL | unwind::DW_EH_PE_SDATA4;
    constexpr std::uint8_t FDE_INS[] = {0x00}; // DW_CFA_nop
    SectionBuilder out;
    EhFrameBuilder eh(out);
    eh.emit_cie(PCREL_SDATA4);
    auto const known = eh.cie_offset();
    eh.emit_fde(0x1000, 0x100, FDE_INS, sizeof(FDE_INS));
    eh.emit_cie_with("zRx", {PCREL_SDATA4});
    eh.emit_fde(0x2000, 0x100, FDE_INS, sizeof(FDE_INS));
    eh.use_cie(known);
    eh.emit_fde(0x3000, 0x100, FDE_INS, sizeof(FDE_INS));
    unwind::Section const section = eh.section();

    std::vector<unwind::FdeIndex::Entry> entries;
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, &collecting_sink, &count));
    EXPECT_EQ(count, 2u);

    unwind::FdeIndex index;
    auto const ordered = sorted(std::move(entries));
    index.init(ordered.data(), ordered.size());
    EXPECT_NE(index.find(0x1080), nullptr);
    EXPECT_EQ(index.find(0x2080), nullptr);
    EXPECT_NE(index.find(0x3080), nullptr);
}

TEST(FdeIndex, RejectsMalformedSection) {
    SectionBuilder out;
    out.u32(0x10).u32(0); // FDE header claiming 16 bytes but truncated
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    std::vector<unwind::FdeIndex::Entry> entries;
    std::size_t count = 99;
    EXPECT_FALSE(unwind::FdeIndex::build(section, 0, &entries, &collecting_sink, &count));
}

TEST(FdeIndex, DebugFrameUsesAbsoluteEncodings) {
    SectionBuilder out;
    // .debug_frame CIE: id is all-ones, augmentation absent, encoding absptr.
    std::size_t const cie_start = out.size();
    // .debug_frame v4: address/segment sizes precede an empty augmentation
    // string.
    out.u32(0).u32(0xFFFF'FFFF).u8(4).u8(8).u8(0).u8(0).uleb(1).sleb(-8).uleb(16);
    out.patch_u32(cie_start, static_cast<std::uint32_t>(out.size() - cie_start - 4));

    std::size_t const fde_start = out.size();
    out.u32(0).u32(0); // length placeholder; cie reference is absolute 0
    out.u64(0x5000).u64(0x40);
    out.patch_u32(fde_start, static_cast<std::uint32_t>(out.size() - fde_start - 4));
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::DebugFrame, 0};

    unwind::EntryKind kind = unwind::EntryKind::Terminator;
    std::uint64_t length = 0;
    ASSERT_TRUE(unwind::identify_entry(section, 0, &kind, &length));
    EXPECT_EQ(kind, unwind::EntryKind::Cie);

    unwind::Cie cie = {};
    ASSERT_TRUE(unwind::parse_cie(section, 0, &cie));
    EXPECT_EQ(cie.version, 4);
    EXPECT_EQ(cie.ra_column, 16u);

    std::vector<unwind::FdeIndex::Entry> entries;
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, &collecting_sink, &count));
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(entries[0].pc_begin, 0x5000u);
    EXPECT_EQ(entries[0].pc_end, 0x5040u);
}

} // namespace
