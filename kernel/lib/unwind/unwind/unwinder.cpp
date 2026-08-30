/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <unwind/unwinder.hpp>

namespace unwind {
namespace {
    // DW_CFA opcodes consumed by the evaluator (DWARF4 Table 25). Anything
    // not listed either advances the location (0x40..0xBF) or is rejected.
    constexpr std::uint8_t DW_CFA_NOP = 0x00;
    constexpr std::uint8_t DW_CFA_SET_LOC = 0x01;
    constexpr std::uint8_t DW_CFA_ADVANCE_LOC1 = 0x02;
    constexpr std::uint8_t DW_CFA_ADVANCE_LOC2 = 0x03;
    constexpr std::uint8_t DW_CFA_ADVANCE_LOC4 = 0x04;
    constexpr std::uint8_t DW_CFA_OFFSET_EXTENDED = 0x05;
    constexpr std::uint8_t DW_CFA_RESTORE_EXTENDED = 0x06;
    constexpr std::uint8_t DW_CFA_UNDEFINED = 0x07;
    constexpr std::uint8_t DW_CFA_SAME_VALUE = 0x08;
    constexpr std::uint8_t DW_CFA_REGISTER = 0x09;
    constexpr std::uint8_t DW_CFA_REMEMBER_STATE = 0x0A;
    constexpr std::uint8_t DW_CFA_RESTORE_STATE = 0x0B;
    constexpr std::uint8_t DW_CFA_DEF_CFA = 0x0C;
    constexpr std::uint8_t DW_CFA_DEF_CFA_REGISTER = 0x0D;
    constexpr std::uint8_t DW_CFA_DEF_CFA_OFFSET = 0x0E;
    constexpr std::uint8_t DW_CFA_DEF_CFA_EXPRESSION = 0x0F;
    constexpr std::uint8_t DW_CFA_EXPRESSION = 0x10;
    constexpr std::uint8_t DW_CFA_OFFSET_EXTENDED_SF = 0x11;
    constexpr std::uint8_t DW_CFA_DEF_CFA_SF = 0x12;
    constexpr std::uint8_t DW_CFA_DEF_CFA_OFFSET_SF = 0x13;
    constexpr std::uint8_t DW_CFA_VAL_OFFSET = 0x14;
    constexpr std::uint8_t DW_CFA_VAL_OFFSET_SF = 0x15;
    constexpr std::uint8_t DW_CFA_VAL_EXPRESSION = 0x16;

    constexpr std::uint8_t DW_CFA_ADVANCE_MASK = 0xC0;
    constexpr std::uint8_t DW_CFA_ADVANCE_LOC = 0x40;

    constexpr std::size_t RULE_COUNT = 17;
    constexpr std::size_t REMEMBER_DEPTH = 4;
    constexpr std::uint64_t MAX_FRAME_DELTA = 0x10'0000;
    constexpr std::uint64_t LOCATION_END = ~std::uint64_t{0};

    enum class RuleKind : std::uint8_t {
        Undefined,
        Same,
        Offset,
        ValOffset,
        Register,
        Unsupported,
    };

    struct Rule final {
        RuleKind kind = RuleKind::Undefined;
        std::uint64_t reg = 0;
        std::int64_t offset = 0;
    };

    struct EvalState final {
        std::uint64_t cfa_reg = Registers::RSP;
        std::int64_t cfa_offset = 8;
        Rule rules[RULE_COUNT] = {};
    };

    auto read_stack(MemoryReader const &memory, std::uint64_t address, std::uint64_t *out) -> bool {
        if ((address & 0x7) != 0) {
            return false;
        }
        return memory.read(memory.context, address, out);
    }

    auto set_rule(EvalState &state, std::uint64_t reg, Rule rule) -> void {
        if (reg < RULE_COUNT) {
            state.rules[reg] = rule;
        }
        // Vector or vendor registers are irrelevant to a CFA walk; their
        // rules are parsed and dropped.
    }

    /// Apply the CFI program in [begin, end) to `*state` until the covered
    /// location would move past `target`. `target` may be `LOCATION_END` for
    /// CIE initial programs, which have no location of their own.
    auto evaluate(
        Section const &section,
        std::uint64_t begin,
        std::uint64_t end,
        std::uint64_t target,
        Cie const &cie,
        EvalState *state
    ) -> bool {
        if (end < begin || end > section.size) {
            return false;
        }

        Cursor cursor(section.data + begin, end - begin);
        EvalState const initial_snapshot = *state;
        EvalState remember_stack[REMEMBER_DEPTH];
        std::size_t remember_depth = 0;
        std::uint64_t location = 0;

        for (std::uint32_t budget = 0; budget < Unwinder::MAX_CFI_OPS; ++budget) {
            if (!cursor.ok()) {
                return false;
            }
            if (cursor.offset() >= end - begin) {
                return true;
            }

            std::uint8_t const opcode = cursor.u8();
            if (!cursor.ok()) {
                return false;
            }

            if ((opcode & DW_CFA_ADVANCE_MASK) == DW_CFA_ADVANCE_LOC) {
                location += static_cast<std::uint64_t>(opcode & 0x3F) * cie.code_align;
                if (location > target) {
                    return true;
                }
                continue;
            }

            // Compact single-register forms: DW_CFA_offset (0x80|reg) and
            // DW_CFA_restore (0xC0|reg) — by far the most emitted opcodes.
            if ((opcode & 0xC0) == 0x80) {
                std::uint64_t const reg = opcode & 0x3F;
                std::int64_t const factored = static_cast<std::int64_t>(cursor.uleb());
                if (!cursor.ok()) {
                    return false;
                }
                set_rule(*state, reg, Rule{RuleKind::Offset, 0, factored});
                continue;
            }
            if ((opcode & 0xC0) == 0xC0) {
                std::uint64_t const reg = opcode & 0x3F;
                if (reg < RULE_COUNT) {
                    state->rules[reg] = initial_snapshot.rules[reg];
                }
                continue;
            }

            switch (opcode) {
                case DW_CFA_NOP:
                    break;

                case DW_CFA_SET_LOC: {
                    std::uint64_t const address = cursor.u64();
                    if (address > target) {
                        return true;
                    }
                    location = address;
                    break;
                }

                case DW_CFA_ADVANCE_LOC1: {
                    location += static_cast<std::uint64_t>(cursor.u8()) * cie.code_align;
                    if (location > target) {
                        return true;
                    }
                    break;
                }

                case DW_CFA_ADVANCE_LOC2: {
                    location += static_cast<std::uint64_t>(cursor.u16()) * cie.code_align;
                    if (location > target) {
                        return true;
                    }
                    break;
                }

                case DW_CFA_ADVANCE_LOC4: {
                    location += static_cast<std::uint64_t>(cursor.u32()) * cie.code_align;
                    if (location > target) {
                        return true;
                    }
                    break;
                }

                case DW_CFA_OFFSET_EXTENDED: {
                    std::uint64_t const reg = cursor.uleb();
                    std::int64_t const factored = static_cast<std::int64_t>(cursor.uleb());
                    set_rule(*state, reg, Rule{RuleKind::Offset, 0, factored});
                    break;
                }

                case DW_CFA_RESTORE_EXTENDED: {
                    std::uint64_t const reg = cursor.uleb();
                    if (reg < RULE_COUNT) {
                        state->rules[reg] = initial_snapshot.rules[reg];
                    }
                    break;
                }

                case DW_CFA_UNDEFINED: {
                    std::uint64_t const reg = cursor.uleb();
                    set_rule(*state, reg, Rule{RuleKind::Undefined, 0, 0});
                    break;
                }

                case DW_CFA_SAME_VALUE: {
                    std::uint64_t const reg = cursor.uleb();
                    set_rule(*state, reg, Rule{RuleKind::Same, 0, 0});
                    break;
                }

                case DW_CFA_REGISTER: {
                    std::uint64_t const reg = cursor.uleb();
                    std::uint64_t const source = cursor.uleb();
                    set_rule(*state, reg, Rule{RuleKind::Register, source, 0});
                    break;
                }

                case DW_CFA_REMEMBER_STATE: {
                    if (remember_depth >= REMEMBER_DEPTH) {
                        return false;
                    }
                    remember_stack[remember_depth] = *state;
                    ++remember_depth;
                    break;
                }

                case DW_CFA_RESTORE_STATE: {
                    if (remember_depth == 0) {
                        return false;
                    }
                    --remember_depth;
                    *state = remember_stack[remember_depth];
                    break;
                }

                case DW_CFA_DEF_CFA: {
                    state->cfa_reg = cursor.uleb();
                    state->cfa_offset = static_cast<std::int64_t>(cursor.uleb());
                    break;
                }

                case DW_CFA_DEF_CFA_REGISTER: {
                    state->cfa_reg = cursor.uleb();
                    break;
                }

                case DW_CFA_DEF_CFA_OFFSET: {
                    state->cfa_offset = static_cast<std::int64_t>(cursor.uleb());
                    break;
                }

                case DW_CFA_OFFSET_EXTENDED_SF: {
                    std::uint64_t const reg = cursor.uleb();
                    std::int64_t const factored = cursor.sleb();
                    set_rule(*state, reg, Rule{RuleKind::Offset, 0, factored});
                    break;
                }

                case DW_CFA_DEF_CFA_SF: {
                    state->cfa_reg = cursor.uleb();
                    state->cfa_offset = cursor.sleb();
                    break;
                }

                case DW_CFA_DEF_CFA_OFFSET_SF: {
                    state->cfa_offset = cursor.sleb();
                    break;
                }

                case DW_CFA_VAL_OFFSET: {
                    std::uint64_t const reg = cursor.uleb();
                    std::int64_t const factored = static_cast<std::int64_t>(cursor.uleb());
                    set_rule(*state, reg, Rule{RuleKind::ValOffset, 0, factored});
                    break;
                }

                case DW_CFA_VAL_OFFSET_SF: {
                    std::uint64_t const reg = cursor.uleb();
                    std::int64_t const factored = cursor.sleb();
                    set_rule(*state, reg, Rule{RuleKind::ValOffset, 0, factored});
                    break;
                }

                case DW_CFA_DEF_CFA_EXPRESSION:
                case DW_CFA_VAL_EXPRESSION:
                    // Call-site expressions need a DWARF expression evaluator.
                    return false;

                case DW_CFA_EXPRESSION: {
                    std::uint64_t const reg = cursor.uleb();
                    std::uint64_t const block_length = cursor.uleb();
                    cursor.skip(block_length);
                    set_rule(*state, reg, Rule{RuleKind::Unsupported, 0, 0});
                    break;
                }

                default:
                    return false;
            }

            if (!cursor.ok()) {
                return false;
            }
        }
        // A real CFI program finishes well inside the budget; hitting the
        // cap means the stream is corrupt or unsupported.
        return false;
    }

    auto slot_address(Cie const &cie, Rule const &rule, std::uint64_t cfa) -> std::uint64_t {
        std::int64_t const bytes = rule.offset * cie.data_align;
        return cfa + static_cast<std::uint64_t>(bytes);
    }
} // namespace

auto Unwinder::locate(std::uint64_t pc, SectionRef *out) const noexcept -> bool {
    if (m_index == nullptr || m_sections == nullptr) {
        return false;
    }
    FdeIndex::Entry const *entry = m_index->find(pc);
    if (entry == nullptr || entry->section >= m_section_count) {
        return false;
    }
    out->entry = entry;
    out->section = &m_sections[entry->section];
    return true;
}

auto Unwinder::step(Registers *regs, MemoryReader const &memory) const -> StepResult {
    SectionRef ref = {};
    if (!locate(regs->pc, &ref)) {
        return StepResult::NoFde;
    }
    Section const &section = *ref.section;
    std::uint64_t const fde_offset = ref.entry->fde_offset;

    // Resolve the FDE's CIE the same way the index build does; linkers group
    // FDEs per CIE, so re-parsing per sample stays cheap and cacheless.
    Cursor probe(section.data + fde_offset + 4, section.size - fde_offset - 4);
    std::uint32_t const cie_reference = probe.u32();
    if (!probe.ok()) {
        return StepResult::BadCfi;
    }
    std::uint64_t const cie_offset = (section.kind == Section::Kind::EhFrame)
                                         ? fde_offset + 4 - cie_reference
                                         : cie_reference;

    Cie cie = {};
    if (!parse_cie(section, cie_offset, &cie)) {
        return StepResult::BadCfi;
    }
    Fde fde = {};
    if (!parse_fde(section, fde_offset, cie, &fde)) {
        return StepResult::BadCfi;
    }

    EvalState state = {};
    if (!evaluate(section, cie.instructions_begin, cie.instructions_end, LOCATION_END, cie, &state)) {
        return StepResult::BadCfi;
    }
    if (!evaluate(section, fde.instructions_begin, fde.instructions_end, regs->pc, cie, &state)) {
        return StepResult::BadCfi;
    }

    std::uint64_t cfa = 0;
    if (state.cfa_reg == Registers::RSP) {
        cfa = regs->sp + static_cast<std::uint64_t>(state.cfa_offset);
    } else if (state.cfa_reg == Registers::RBP) {
        cfa = regs->bp + static_cast<std::uint64_t>(state.cfa_offset);
    } else {
        return StepResult::BadCfi;
    }

    Rule const &sp_rule = state.rules[Registers::RSP];
    std::uint64_t new_sp = 0;
    switch (sp_rule.kind) {
        case RuleKind::Undefined:
            // The caller's SP is the CFA itself on the SysV x86-64 ABI.
            new_sp = cfa;
            break;
        case RuleKind::Same:
            new_sp = regs->sp;
            break;
        case RuleKind::Offset:
            if (!read_stack(memory, slot_address(cie, sp_rule, cfa), &new_sp)) {
                return StepResult::BadFrame;
            }
            break;
        case RuleKind::ValOffset:
            new_sp = slot_address(cie, sp_rule, cfa);
            break;
        default:
            return StepResult::BadCfi;
    }

    std::uint64_t ra_column = static_cast<std::uint64_t>(Registers::RIP);
    if (cie.ra_column < RULE_COUNT) {
        ra_column = cie.ra_column;
    }
    Rule const &ra_rule = state.rules[ra_column];
    std::uint64_t new_pc = 0;
    switch (ra_rule.kind) {
        case RuleKind::Offset:
            if (!read_stack(memory, slot_address(cie, ra_rule, cfa), &new_pc)) {
                return StepResult::BadFrame;
            }
            break;
        case RuleKind::ValOffset:
            new_pc = slot_address(cie, ra_rule, cfa);
            break;
        default:
            // `Same` marks signal/trap frames (no return address was pushed)
            // and everything else cannot be recovered from CFI alone.
            return StepResult::BadFrame;
    }

    Rule const &bp_rule = state.rules[Registers::RBP];
    std::uint64_t new_bp = 0;
    switch (bp_rule.kind) {
        case RuleKind::Same:
            new_bp = regs->bp;
            break;
        case RuleKind::Offset:
        case RuleKind::ValOffset:
            new_bp = slot_address(cie, bp_rule, cfa);
            if (bp_rule.kind == RuleKind::Offset &&
                !read_stack(memory, new_bp, &new_bp)) {
                return StepResult::BadFrame;
            }
            break;
        case RuleKind::Undefined:
            // The caller does not keep a frame pointer we can recover; a
            // later CFA rule that needs it will fail the next step's read.
            new_bp = 0;
            break;
        default:
            return StepResult::BadCfi;
    }

    if (new_sp <= regs->sp || new_sp - regs->sp > MAX_FRAME_DELTA) {
        return StepResult::BadFrame;
    }
    if ((new_sp & 0x7) != 0 || new_pc == 0) {
        return StepResult::BadFrame;
    }

    regs->pc = new_pc;
    regs->sp = new_sp;
    regs->bp = new_bp;
    return StepResult::Ok;
}

} // namespace unwind
