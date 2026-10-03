/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/ring.hpp>

#include <atomic>

#include <cstring>

namespace logz4::ring {
namespace {
    enum class SlotState : u32 {
        Free = 0,
        Writing = 1,
        Committed = 2,
    };

    /// One fixed-size record. The state machine gives exclusive ownership
    /// of a slot to exactly one producer at a time: Free/Committed may be
    /// claimed by a CAS to Writing, and a reader only ever trusts
    /// Committed slots whose ticket matches the position being replayed --
    /// so a record being overwritten mid-dump is skipped, never torn into
    /// the output.
    struct Slot final {
        std::atomic<u32> state;
        u32 cpu;
        std::atomic<u64> ticket;
        Time time;
        Domain const *domain;
        Level level;
        u16 len;
        char bytes[LINE_BYTES];
    };

    static_assert(sizeof(Slot) == 552, "Slot layout is part of the .bss budget");

    /// Claimed in order by fetch_add; the slot is `ticket % SLOT_COUNT`.
    /// Monotonic, so replay order is reconstructed even after wraps.
    std::atomic<u64> next_ticket{0};

    Slot slots[SLOT_COUNT];
} // namespace

auto append(char const *line, usize len, Time time, u32 cpu, Level level, Domain const *domain)
    -> void {
    auto const ticket = next_ticket.fetch_add(1, std::memory_order_acq_rel);
    auto &slot = slots[ticket % SLOT_COUNT];

    // Claim the slot. Spinning on Writing is bounded in practice: it means
    // another producer is mid-memcpy on this same slot, which requires
    // SLOT_COUNT other lines to have been appended in the meantime.
    auto observed = slot.state.load(std::memory_order_acquire);
    while (true) {
        while (observed == static_cast<u32>(SlotState::Writing)) {
            observed = slot.state.load(std::memory_order_acquire);
        }
        if (slot.state.compare_exchange_weak(observed, static_cast<u32>(SlotState::Writing),
                                             std::memory_order_acq_rel)) {
            break;
        }
    }

    slot.ticket.store(ticket, std::memory_order_relaxed);
    slot.time = time;
    slot.cpu = cpu;
    slot.level = level;
    slot.domain = domain;
    auto stored_len = len;
    if (stored_len > LINE_BYTES) {
        stored_len = LINE_BYTES;
    }
    slot.len = static_cast<u16>(stored_len);
    if (slot.len != 0) {
        std::memcpy(slot.bytes, line, slot.len);
    }

    slot.state.store(static_cast<u32>(SlotState::Committed), std::memory_order_release);
}

namespace {
    /// Every dump in one loop. `only` null means every record, which is not
    /// the same as "the anonymous domain's records" -- nothing asks for
    /// those alone, so the null needs no second meaning. `newest` caps how
    /// many of the latest tickets are looked at; SLOT_COUNT means all.
    auto replay(Logger &sink, Domain const *only, u64 newest) -> usize {
        auto const next = next_ticket.load(std::memory_order_acquire);
        if (newest > SLOT_COUNT) {
            newest = SLOT_COUNT;
        }
        auto first = u64{0};
        if (next > newest) {
            first = next - newest;
        }

        usize replayed = 0;
        char copy[LINE_BYTES];
        for (auto ticket = first; ticket < next; ++ticket) {
            auto const &slot = slots[ticket % SLOT_COUNT];
            if (slot.state.load(std::memory_order_acquire) !=
                static_cast<u32>(SlotState::Committed)) {
                continue;
            }
            // A committed slot whose ticket disagrees with the position being
            // replayed has been claimed by a newer producer: its bytes belong
            // to the future, not to this position.
            if (slot.ticket.load(std::memory_order_relaxed) != ticket) {
                continue;
            }
            auto const len = slot.len;
            if (len > LINE_BYTES) {
                continue;
            }
            // Like `len`, read after the first check and vouched for by the
            // recheck below: a domain taken from a slot that is being
            // overwritten is thrown away together with that slot's bytes.
            if (only != nullptr && slot.domain != only) {
                continue;
            }
            if (len != 0) {
                std::memcpy(copy, slot.bytes, len);
            }
            // Recheck after the copy: a producer that claimed this slot
            // mid-memcpy would have moved state off Committed or changed the
            // ticket, and the bytes in `copy` would be torn.
            if (slot.state.load(std::memory_order_acquire) !=
                static_cast<u32>(SlotState::Committed)) {
                continue;
            }
            if (slot.ticket.load(std::memory_order_relaxed) != ticket) {
                continue;
            }
            if (len != 0) {
                sink.write(copy, len);
            }
            ++replayed;
        }
        return replayed;
    }
} // namespace

auto dump_to(Logger &sink) -> usize {
    return replay(sink, nullptr, SLOT_COUNT);
}

auto dump_to(Logger &sink, Domain const &only) -> usize {
    return replay(sink, &only, SLOT_COUNT);
}

auto dump_tail_to(Logger &sink, usize count) -> usize {
    return replay(sink, nullptr, count);
}

auto reset_for_test() -> void {
    for (auto &slot: slots) {
        slot.state.store(static_cast<u32>(SlotState::Free), std::memory_order_relaxed);
        slot.len = 0;
    }
    next_ticket.store(0, std::memory_order_relaxed);
}
} // namespace logz4::ring
