/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <unwind/fde_index.hpp>

namespace unwind {
namespace {
    constexpr std::uint64_t NO_CIE_CACHE = ~std::uint64_t{0};
} // namespace

auto FdeIndex::build(Section const &section, std::uint32_t section_id, void *context, Sink sink,
                     std::size_t *count) -> bool {
    Cie cached_cie = {};
    std::uint64_t cached_cie_offset = NO_CIE_CACHE;
    std::size_t produced = 0;

    std::uint64_t offset = 0;
    while (offset < section.size) {
        EntryKind kind = EntryKind::Terminator;
        std::uint64_t length = 0;
        if (!identify_entry(section, offset, &kind, &length)) {
            return false;
        }
        if (kind == EntryKind::Terminator) {
            break;
        }
        // A record's declared length is the only thing that tells the parsers
        // where its instruction stream ends (`offset + length` becomes
        // `instructions_end`), and the unwinder later evaluates up to that
        // end. Every Cursor read is bounded by the section, so a length that
        // runs past it cannot make the walk itself read out of bounds -- but
        // it would index an FDE whose program is truncated. Reject the
        // section instead; `offset < size` holds here, so no underflow.
        if (length > section.size - offset) {
            return false;
        }

        if (kind == EntryKind::Cie) {
            Cie cie = {};
            if (!parse_cie(section, offset, &cie)) {
                return false;
            }
            cached_cie = cie;
            cached_cie_offset = offset;
        } else {
            // The FDE references its CIE by pointer; keep the last parsed CIE
            // around since linkers group FDEs per CIE, making the cache hit
            // rate close to 100%.
            Cursor probe(section.data + offset + 4, section.size - offset - 4);
            std::uint32_t const cie_reference = probe.u32();
            if (!probe.ok()) {
                return false;
            }
            std::uint64_t const cie_offset = (section.kind == Section::Kind::EhFrame)
                                                 ? offset + 4 - cie_reference
                                                 : cie_reference;
            if (cie_offset != cached_cie_offset) {
                if (!parse_cie(section, cie_offset, &cached_cie)) {
                    return false;
                }
                cached_cie_offset = cie_offset;
            }

            Fde fde = {};
            if (!parse_fde(section, offset, cached_cie, &fde)) {
                return false;
            }
            if (fde.pc_range != 0) {
                Entry const entry{fde.pc_begin, fde.pc_begin + fde.pc_range, offset, section_id};
                if (!sink(context, entry)) {
                    return false;
                }
                ++produced;
            }
        }

        offset += length;
    }

    *count = produced;
    return true;
}

auto FdeIndex::find(std::uint64_t pc) const noexcept -> Entry const * {
    if (m_count == 0 || m_entries == nullptr) {
        return nullptr;
    }

    // Rightmost entry with pc_begin <= pc, then a containment check: nested
    // or duplicate ranges resolve to the narrowest match only when the
    // array was sorted with exact ranges first — callers sorting on
    // (pc_begin, pc_end) ascending get first-match-wins on ties.
    std::size_t low = 0;
    std::size_t high = m_count;
    while (low < high) {
        std::size_t const mid = low + (high - low) / 2;
        if (m_entries[mid].pc_begin <= pc) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == 0) {
        return nullptr;
    }

    Entry const *candidate = &m_entries[low - 1];
    if (pc >= candidate->pc_end) {
        return nullptr;
    }
    return candidate;
}

} // namespace unwind
