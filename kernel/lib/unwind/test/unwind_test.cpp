/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <unwind/fde_index.hpp>
#include <unwind/unwinder.hpp>

#include <algorithm>
#include <utility>
#include <vector>

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

// A minimal .eh_frame emitter shaped like x86-64 clang output, plus a
// single-region fake memory backing stack-slot reads. Opcode constants are
// spelled locally: the evaluator keeps its table private.
constexpr std::uint8_t CFA_NOP = 0x00;
constexpr std::uint8_t CFA_DEF_CFA = 0x0C;
constexpr std::uint8_t CFA_DEF_CFA_REGISTER = 0x0D;
constexpr std::uint8_t CFA_DEF_CFA_OFFSET = 0x0E;
constexpr std::uint8_t CFA_DEF_CFA_SF = 0x12;
constexpr std::uint8_t CFA_OFFSET = 0x80;  // + reg, uleb factored offset
constexpr std::uint8_t CFA_RESTORE = 0xC0; // + reg
constexpr std::uint8_t CFA_ADVANCE = 0x40; // + byte count
constexpr std::uint8_t CFA_REMEMBER_STATE = 0x0A;
constexpr std::uint8_t CFA_RESTORE_STATE = 0x0B;
constexpr std::uint8_t ENC_PCREL_SDATA4 = 0x1B; // DW_EH_PE_pcrel | DW_EH_PE_sdata4

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
            .uleb(16) // return address column
            .uleb(1)  // augmentation data length
            .u8(ENC_PCREL_SDATA4)
            .u8(CFA_DEF_CFA)
            .uleb(7)
            .uleb(8)
            .u8(CFA_OFFSET | 16)
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

class FakeMemory final {
  public:
    auto add_region(std::uint64_t base, std::size_t size) -> void {
        m_bytes.assign(size, 0);
        m_base = base;
    }

    auto write(std::uint64_t address, std::uint64_t value) -> void {
        for (std::size_t i = 0; i < 8; ++i) {
            m_bytes.at(address - m_base + i) = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
    }

    static auto read(void *context, std::uint64_t address, std::uint64_t *out) -> bool {
        auto *self = static_cast<FakeMemory *>(context);
        if (address < self->m_base || address + 8 > self->m_base + self->m_bytes.size()) {
            return false;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(self->m_bytes[address - self->m_base + i])
                     << (8 * i);
        }
        *out = value;
        return true;
    }

    [[nodiscard]]
    auto reader() -> unwind::MemoryReader {
        return unwind::MemoryReader{this, &FakeMemory::read};
    }

  private:
    std::uint64_t m_base = 0;
    std::vector<std::uint8_t> m_bytes;
};

// The two-function chain under test:
//
//   C <-- B <-- A (sampled inside A's body)
//
// A executes `push rbp; mov rbp, rsp` (frame pointer established); B runs
// with the CIE's default (no-prologue) rules. With R = A's caller stack
// pointer at the call site: the interrupted SP/rbp equal R - 16, A's CFA is
// R, the return address into B sits at R - 8 and the saved rbp at R - 16.
class ChainHarness final {
  public:
    static constexpr std::uint64_t A_PC = 0x1000;
    static constexpr std::uint64_t B_PC = 0x2000;
    static constexpr std::uint64_t A_RET = B_PC + 0x42;
    static constexpr std::uint64_t B_RET = 0x3000 + 0x10;
    static constexpr std::uint64_t STACK_BASE = 0x7FFF'0000;
    static constexpr std::uint64_t A_SP = STACK_BASE + 0x100; // interrupted SP = rbp

    ChainHarness() {
        m_eh.emit_cie();

        // A: advance 1 (push rbp); def_cfa_offset 16; offset(rbp=6, factored
        // 2); advance 3 (mov rbp, rsp); def_cfa_register(rbp=6).
        constexpr std::uint8_t A_INS[] = {
            CFA_ADVANCE | 1, CFA_DEF_CFA_OFFSET,   16, CFA_OFFSET | 6, 2,
            CFA_ADVANCE | 3, CFA_DEF_CFA_REGISTER, 6,
        };
        m_eh.emit_fde(A_PC, 0x100, A_INS, sizeof(A_INS));

        // B: empty program — the CIE initial rules apply everywhere.
        constexpr std::uint8_t B_INS[] = {CFA_NOP};
        m_eh.emit_fde(B_PC, 0x100, B_INS, sizeof(B_INS));

        m_section = m_eh.section();
        auto const sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
            static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
            return true;
        };
        std::size_t count = 0;
        m_build_ok = unwind::FdeIndex::build(m_section, 0, &m_entries, sink, &count);
        m_build_count = count;
        m_index.init(m_entries.data(), m_entries.size());

        // A_SP + 0: saved caller rbp; + 8: return address into B;
        // + 16: return address into C (visible from B's frame).
        m_stack.add_region(STACK_BASE, 0x200);
        m_stack.write(A_SP, 0x7FFF'0800);
        m_stack.write(A_SP + 8, A_RET);
        m_stack.write(A_SP + 16, B_RET);
    }

    [[nodiscard]]
    auto unwinder() const -> unwind::Unwinder {
        return unwind::Unwinder(&m_section, 1, &m_index);
    }

    [[nodiscard]]
    auto memory() -> unwind::MemoryReader {
        return m_stack.reader();
    }

    // Interrupted inside A's body: pc after the prologue, sp and rbp both at
    // the interrupted value.
    [[nodiscard]]
    auto initial_registers() const -> unwind::Registers {
        return unwind::Registers{.pc = A_PC + 4, .sp = A_SP, .bp = A_SP};
    }

    [[nodiscard]]
    auto section() const -> unwind::Section {
        return m_section;
    }

    [[nodiscard]]
    auto build_ok() const -> bool {
        return m_build_ok;
    }

    [[nodiscard]]
    auto build_count() const -> std::size_t {
        return m_build_count;
    }

  private:
    FakeEhFrame m_eh;
    bool m_build_ok = false;
    std::size_t m_build_count = 0;
    unwind::Section m_section{};
    std::vector<unwind::FdeIndex::Entry> m_entries;
    unwind::FdeIndex m_index;
    FakeMemory m_stack;
};

TEST(Unwinder, WalksCallChainToLeafAndStopsWithoutFde) {
    ChainHarness harness;

    ASSERT_TRUE(harness.build_ok());
    ASSERT_EQ(harness.build_count(), 2u);

    unwind::Registers regs = harness.initial_registers();
    unwind::Unwinder const unwinder = harness.unwinder();

    // Frame A (frame pointer established): CFA = rbp + 16 = A_SP + 16.
    EXPECT_EQ(unwinder.step(&regs, harness.memory()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, ChainHarness::A_RET);
    EXPECT_EQ(regs.sp, ChainHarness::A_SP + 16);
    EXPECT_EQ(regs.bp, std::uint64_t{0x7FFF'0800});

    // Frame B (no prologue): CFA = sp + 8 = A_SP + 24.
    EXPECT_EQ(unwinder.step(&regs, harness.memory()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, ChainHarness::B_RET);
    EXPECT_EQ(regs.sp, ChainHarness::A_SP + 24);
    // B never touches rbp, so its caller's is the one A restored.
    EXPECT_EQ(regs.bp, std::uint64_t{0x7FFF'0800});

    // Frame C has no FDE: it is the last trustworthy frame.
    EXPECT_EQ(unwinder.step(&regs, harness.memory()), unwind::StepResult::NoFde);
    EXPECT_EQ(regs.pc, ChainHarness::B_RET);
}

// A function whose last CFI row is its epilogue's, as the compiler emits for
// one return at the end: push rbp; mov rbp, rsp; ...body...; pop rbp (the CFA
// back to rsp + 8); ret. A PC in the body is governed by the rows up to it,
// not by the epilogue row after it. The chain above never had a row after
// its sampled PC, so every row applying and the right rows applying gave the
// same answer there.
TEST(Unwinder, RowsAfterThePcDoNotApply) {
    constexpr std::uint64_t F_PC = 0x1000;
    constexpr std::uint64_t CALLER_RET = 0x2042;
    constexpr std::uint64_t CALLER_RBP = 0x7FFF'0800;
    constexpr std::uint64_t STACK_BASE = 0x7FFF'0000;
    constexpr std::uint64_t RBP = STACK_BASE + 0x100; // after mov rbp, rsp
    constexpr std::uint64_t SP = STACK_BASE + 0x80;   // the body's locals below

    FakeEhFrame eh;
    eh.emit_cie();
    // advance 1 (push rbp): def_cfa_offset 16, offset(rbp, factored 2);
    // advance 3 (mov rbp, rsp): def_cfa_register(rbp);
    // advance 0x3c, to F_PC + 0x40 (pop rbp done, ret next): def_cfa(rsp, 8).
    constexpr std::uint8_t F_INS[] = {
        CFA_ADVANCE | 1,
        CFA_DEF_CFA_OFFSET,
        16,
        CFA_OFFSET | 6,
        2,
        CFA_ADVANCE | 3,
        CFA_DEF_CFA_REGISTER,
        6,
        CFA_ADVANCE | 0x3c,
        CFA_DEF_CFA,
        7,
        8,
    };
    eh.emit_fde(F_PC, 0x48, F_INS, sizeof(F_INS));
    unwind::Section const section = eh.section();

    std::vector<unwind::FdeIndex::Entry> entries;
    auto const sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
        static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
        return true;
    };
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, sink, &count));
    unwind::FdeIndex index;
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(STACK_BASE, 0x200);
    stack.write(RBP, CALLER_RBP);
    stack.write(RBP + 8, CALLER_RET);
    stack.write(SP, 0xDEAD'BEEF); // what the body keeps at the top of its stack

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::Registers regs{.pc = F_PC + 0x10, .sp = SP, .bp = RBP};
    ASSERT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, CALLER_RET);
    EXPECT_EQ(regs.sp, RBP + 16);
    EXPECT_EQ(regs.bp, CALLER_RBP);

    // At the ret itself the epilogue row does apply: CFA = rsp + 8.
    stack.write(SP, CALLER_RET);
    unwind::Registers at_ret{.pc = F_PC + 0x40, .sp = SP, .bp = CALLER_RBP};
    ASSERT_EQ(unwinder.step(&at_ret, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(at_ret.pc, CALLER_RET);
    EXPECT_EQ(at_ret.sp, SP + 8);
}

// A leaf that never touches rbp -- no frame pointer of its own, the CIE's
// rules only -- called from a function with one. The leaf's CFI says
// nothing about rbp, which means it left it alone: unwinding the leaf must
// hand the caller its own rbp back, or the caller's rbp-based CFA cannot be
// computed and the walk stops one frame short.
TEST(Unwinder, AFrameThatNeverSavesRbpLeavesItForItsCaller) {
    constexpr std::uint64_t L_PC = 0x1000; // the leaf
    constexpr std::uint64_t F_PC = 0x2000; // its caller, with a frame pointer
    constexpr std::uint64_t L_RET = F_PC + 0x10;
    constexpr std::uint64_t F_RET = 0x3042;
    constexpr std::uint64_t F_CALLER_RBP = 0x7FFF'0800;
    constexpr std::uint64_t STACK_BASE = 0x7FFF'0000;
    constexpr std::uint64_t L_SP = STACK_BASE + 0x80;
    constexpr std::uint64_t F_RBP = STACK_BASE + 0x100;

    FakeEhFrame eh;
    eh.emit_cie();
    constexpr std::uint8_t L_INS[] = {CFA_NOP};
    eh.emit_fde(L_PC, 0x20, L_INS, sizeof(L_INS));
    constexpr std::uint8_t F_INS[] = {
        CFA_ADVANCE | 1, CFA_DEF_CFA_OFFSET,   16, CFA_OFFSET | 6, 2,
        CFA_ADVANCE | 3, CFA_DEF_CFA_REGISTER, 6,
    };
    eh.emit_fde(F_PC, 0x100, F_INS, sizeof(F_INS));
    unwind::Section const section = eh.section();

    std::vector<unwind::FdeIndex::Entry> entries;
    auto const sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
        static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
        return true;
    };
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, sink, &count));
    std::sort(entries.begin(), entries.end(),
              [](auto const &a, auto const &b) { return a.pc_begin < b.pc_begin; });
    unwind::FdeIndex index;
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(STACK_BASE, 0x200);
    stack.write(L_SP, L_RET);         // the leaf's return address
    stack.write(F_RBP, F_CALLER_RBP); // the caller's saved rbp
    stack.write(F_RBP + 8, F_RET);    // and its return address

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::Registers regs{.pc = L_PC + 4, .sp = L_SP, .bp = F_RBP};
    ASSERT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, L_RET);
    EXPECT_EQ(regs.sp, L_SP + 8);
    EXPECT_EQ(regs.bp, F_RBP); // untouched by the leaf

    ASSERT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, F_RET);
    EXPECT_EQ(regs.sp, F_RBP + 16);
    EXPECT_EQ(regs.bp, F_CALLER_RBP);
}

// A caller whose last instruction is the call: the return address is the
// caller's end, which is where the next function's FDE begins. The caller's
// rules hold at the call, one byte back; the next function's, which differ,
// must not be used, and a walk must not stop for want of an FDE either.
TEST(Unwinder, ACallThatEndsAFunctionUnwindsWithThatFunctionsRules) {
    constexpr std::uint64_t L_PC = 0x1000;  // the leaf, CIE rules only
    constexpr std::uint64_t F_PC = 0x2000;  // the caller, rbp-based
    constexpr std::uint64_t F_END = 0x2010; // its call is its last instruction
    constexpr std::uint64_t G_PC = F_END;   // the next function starts there
    constexpr std::uint64_t F_RET = 0x3042;
    constexpr std::uint64_t F_CALLER_RBP = 0x7FFF'0800;
    constexpr std::uint64_t STACK_BASE = 0x7FFF'0000;
    constexpr std::uint64_t L_SP = STACK_BASE + 0x80;
    constexpr std::uint64_t F_RBP = STACK_BASE + 0x100;

    FakeEhFrame eh;
    eh.emit_cie();
    constexpr std::uint8_t L_INS[] = {CFA_NOP};
    eh.emit_fde(L_PC, 0x20, L_INS, sizeof(L_INS));
    constexpr std::uint8_t F_INS[] = {
        CFA_ADVANCE | 1, CFA_DEF_CFA_OFFSET,   16, CFA_OFFSET | 6, 2,
        CFA_ADVANCE | 3, CFA_DEF_CFA_REGISTER, 6,
    };
    eh.emit_fde(F_PC, F_END - F_PC, F_INS, sizeof(F_INS));
    constexpr std::uint8_t G_INS[] = {CFA_NOP}; // CIE rules: CFA = rsp + 8
    eh.emit_fde(G_PC, 0x20, G_INS, sizeof(G_INS));
    unwind::Section const section = eh.section();

    std::vector<unwind::FdeIndex::Entry> entries;
    auto const sink = [](void *context, unwind::FdeIndex::Entry const &entry) -> bool {
        static_cast<std::vector<unwind::FdeIndex::Entry> *>(context)->push_back(entry);
        return true;
    };
    std::size_t count = 0;
    ASSERT_TRUE(unwind::FdeIndex::build(section, 0, &entries, sink, &count));
    std::sort(entries.begin(), entries.end(),
              [](auto const &a, auto const &b) { return a.pc_begin < b.pc_begin; });
    unwind::FdeIndex index;
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(STACK_BASE, 0x200);
    stack.write(L_SP, F_END);      // the leaf returns to the caller's end
    stack.write(L_SP + 8, 0xBAD0); // what G's rules would take for a return address
    stack.write(F_RBP, F_CALLER_RBP);
    stack.write(F_RBP + 8, F_RET);

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::Registers regs{.pc = L_PC + 4, .sp = L_SP, .bp = F_RBP};
    ASSERT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, F_END);
    EXPECT_TRUE(regs.pc_is_return_address);

    ASSERT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::Ok);
    EXPECT_EQ(regs.pc, F_RET);
    EXPECT_EQ(regs.sp, F_RBP + 16);
    EXPECT_EQ(regs.bp, F_CALLER_RBP);
}

TEST(Unwinder, LeavesRegistersUntouchedOnUnreadableMemory) {
    ChainHarness harness;

    unwind::Registers regs = harness.initial_registers();
    FakeMemory unmapped; // nothing mapped: every read fails

    EXPECT_EQ(harness.unwinder().step(&regs, unmapped.reader()), unwind::StepResult::BadFrame);
    EXPECT_EQ(regs.pc, ChainHarness::A_PC + 4);
    EXPECT_EQ(regs.sp, ChainHarness::A_SP);
}

TEST(Unwinder, RejectsCfaBelowStackPointer) {
    // def_cfa(rsp, -64): the computed CFA lands below the current SP, which
    // would walk the stack backwards — refuse it.
    SectionBuilder out;
    std::size_t const cie_start = out.size();
    out.u32(0).u32(0).u8(1).u8(0).uleb(1).sleb(-8).uleb(16);
    out.u8(CFA_DEF_CFA_SF).uleb(7).sleb(-64).u8(CFA_OFFSET | 16).uleb(1);
    out.patch_u32(cie_start, static_cast<std::uint32_t>(out.size() - cie_start - 4));

    std::size_t const fde_start = out.size();
    out.u32(0).u32(static_cast<std::uint32_t>(fde_start + 4 - cie_start));
    out.u64(0x1000).u64(0x100); // absptr: no 'z' augmentation in the CIE
    out.patch_u32(fde_start, static_cast<std::uint32_t>(out.size() - fde_start - 4));
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    unwind::FdeIndex index;
    std::vector<unwind::FdeIndex::Entry> entries;
    entries.push_back(unwind::FdeIndex::Entry{0x1000, 0x1100, fde_start, 0});
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(0x7FFF'0000, 0x200);

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::Registers regs{.pc = 0x1020, .sp = 0x7FFF'0100, .bp = 0x7FFF'0100};
    EXPECT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::BadFrame);
}

TEST(Unwinder, RejectsUnalignedCallerStackPointer) {
    // def_cfa(rsp, 9): the caller SP would be misaligned, which the SysV ABI
    // never produces — treat it as corrupt frame data.
    SectionBuilder out;
    std::size_t const cie_start = out.size();
    out.u32(0).u32(0).u8(1).u8(0).uleb(1).sleb(-8).uleb(16);
    out.u8(CFA_DEF_CFA).uleb(7).uleb(9).u8(CFA_OFFSET | 16).uleb(1);
    out.patch_u32(cie_start, static_cast<std::uint32_t>(out.size() - cie_start - 4));

    std::size_t const fde_start = out.size();
    out.u32(0).u32(static_cast<std::uint32_t>(fde_start + 4 - cie_start));
    out.u64(0x1000).u64(0x100); // absptr: no 'z' augmentation in the CIE
    out.patch_u32(fde_start, static_cast<std::uint32_t>(out.size() - fde_start - 4));
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    unwind::FdeIndex index;
    std::vector<unwind::FdeIndex::Entry> entries;
    entries.push_back(unwind::FdeIndex::Entry{0x1000, 0x1100, fde_start, 0});
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(0x7FFF'0000, 0x200);

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::Registers regs{.pc = 0x1020, .sp = 0x7FFF'0100, .bp = 0x7FFF'0100};
    EXPECT_EQ(unwinder.step(&regs, stack.reader()), unwind::StepResult::BadFrame);
}

TEST(Unwinder, RememberAndRestoreStateRoundTrip) {
    // Epilogue CFI: rules diverge after remember_state and are restored;
    // a PC in the restored region must unwind exactly like the body.
    SectionBuilder out;
    std::size_t const cie_start = out.size();
    out.u32(0).u32(0).u8(1).u8(0).uleb(1).sleb(-8).uleb(16);
    out.u8(CFA_DEF_CFA).uleb(7).uleb(8).u8(CFA_OFFSET | 16).uleb(1);
    out.patch_u32(cie_start, static_cast<std::uint32_t>(out.size() - cie_start - 4));

    std::size_t const fde_start = out.size();
    out.u32(0).u32(static_cast<std::uint32_t>(fde_start + 4 - cie_start));
    out.u64(0x1000).u64(0x100); // absptr: no 'z' augmentation in the CIE
    // Body: advance 1; def_cfa_offset 16; offset(rbp, 2).
    out.u8(CFA_ADVANCE | 1).u8(CFA_DEF_CFA_OFFSET).uleb(16).u8(CFA_OFFSET | 6).uleb(2);
    // Epilogue: remember; advance 1; def_cfa_offset 8; restore(rbp);
    // advance 1; restore_state.
    out.u8(CFA_REMEMBER_STATE)
        .u8(CFA_ADVANCE | 1)
        .u8(CFA_DEF_CFA_OFFSET)
        .uleb(8)
        .u8(CFA_RESTORE | 6)
        .u8(CFA_ADVANCE | 1)
        .u8(CFA_RESTORE_STATE);
    out.patch_u32(fde_start, static_cast<std::uint32_t>(out.size() - fde_start - 4));
    unwind::Section const section{out.data(), out.size(), unwind::Section::Kind::EhFrame, 0};

    unwind::FdeIndex index;
    std::vector<unwind::FdeIndex::Entry> entries;
    entries.push_back(unwind::FdeIndex::Entry{0x1000, 0x1100, fde_start, 0});
    index.init(entries.data(), entries.size());

    FakeMemory stack;
    stack.add_region(0x7FFF'0000, 0x200);
    // With sp = rbp = 0x7FFF'00F8 the CFA is 0x7FFF'0108: the saved rbp sits
    // at CFA-16 and the return address at CFA-8.
    stack.write(0x7FFF'00F8, 0x7FFF'0200); // saved rbp
    stack.write(0x7FFF'0100, 0xCAFEBABE);  // return address

    unwind::Unwinder const unwinder(&section, 1, &index);
    unwind::MemoryReader const reader = stack.reader();

    // Body region: CFA = rbp + 16.
    unwind::Registers body{.pc = 0x1004, .sp = 0x7FFF'00F8, .bp = 0x7FFF'00F8};
    EXPECT_EQ(unwinder.step(&body, reader), unwind::StepResult::Ok);
    EXPECT_EQ(body.pc, std::uint64_t{0xCAFEBABE});
    EXPECT_EQ(body.sp, std::uint64_t{0x7FFF'0108});
    EXPECT_EQ(body.bp, std::uint64_t{0x7FFF'0200});

    // Restored region (past restore_state): identical rules, identical walk.
    unwind::Registers restored{.pc = 0x1006, .sp = 0x7FFF'00F8, .bp = 0x7FFF'00F8};
    EXPECT_EQ(unwinder.step(&restored, reader), unwind::StepResult::Ok);
    EXPECT_EQ(restored.pc, body.pc);
    EXPECT_EQ(restored.sp, body.sp);
    EXPECT_EQ(restored.bp, body.bp);
}

} // namespace
