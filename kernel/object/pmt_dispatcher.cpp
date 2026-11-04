/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/object/pmt_dispatcher.hpp>

#include <algorithm>
#include <utility>

#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::object {
PmtDispatcher::PmtDispatcher(ustl::Arc<BtiDispatcher> bti) : Base(), m_bti(ustl::move(bti)) {}

auto PmtDispatcher::attach(BtiDispatcher::Pin &pin) -> void {
    Guard guard{get_lock()};
    DEBUG_ASSERT(m_pin == nullptr, "a token is attached to a pin twice");
    m_pin = &pin;
}

auto PmtDispatcher::query_address(u64 const offset, u64 const size, Address &out) -> Status {
    m_canary.verify<Self>();

    if (size == 0 || !ustl::is_aligned(offset, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    // Held across the lookups, which is what keeps the pin alive: it is freed
    // by an unpin, which cannot take it from this token before this returns.
    Guard guard{get_lock()};
    if (m_pin == nullptr) {
        return Status::BadState;
    }
    auto &pin = *m_pin;
    // Compared so that nothing overflows: both are at most the pin's size
    // after the first two checks.
    if (size > pin.size || offset > pin.size - size) {
        return Status::OutOfRange;
    }

    return run_at_locked(pin, offset, size, out);
}

auto PmtDispatcher::run_at_locked(BtiDispatcher::Pin &pin, u64 const offset, u64 const size,
                                  Address &out) -> Status {
    // The whole range as one run, and if that is not one, its first page.
    // Rounding up cannot leave the pin: it is a whole number of pages.
    auto length = ustl::round_up(size, PAGE_SIZE);
    auto first = PhysAddr{0};
    auto status = pin.vmo->lookup_contiguous(pin.offset + offset, length, &first);
    if (status == Status::BadState && length > PAGE_SIZE) {
        length = PAGE_SIZE;
        status = pin.vmo->lookup_contiguous(pin.offset + offset, length, &first);
    }
    if (status == Status::Ok) {
        out = {.address = std::to_underlying(first), .size = length};
        return Status::Ok;
    }
    if (status == Status::BadState) {
        // One pinned page is one run and is in its VMO, so this is a page that
        // vanished from under a pin: the kernel's fault and not the caller's.
        // Answered, not panicked on, since a system call reaches it.
        log::error("pmt: a pinned page at offset {:#x} of its VMO is not there",
                   pin.offset + offset);
        return Status::InternalError;
    }
    return status;
}

auto PmtDispatcher::query_pieces(u64 const piece_size, u64 const first_piece,
                                 std::span<PhysAddr> const out) -> Status {
    m_canary.verify<Self>();

    if (piece_size == 0 || !ustl::is_aligned(piece_size, PAGE_SIZE)) {
        return Status::InvArgs;
    }

    Guard guard{get_lock()};
    if (m_pin == nullptr) {
        return Status::BadState;
    }
    auto &pin = *m_pin;

    for (usize at = 0; at < out.size(); ++at) {
        // The piece's start, compared as an index so that nothing overflows:
        // the last piece that starts inside the pin has this index.
        auto const index = first_piece + at;
        if (index < first_piece || index > (pin.size - 1) / piece_size) {
            return Status::OutOfRange;
        }
        auto const start = index * piece_size;
        auto const length = std::min(piece_size, pin.size - start);

        // Each piece asks about its own bytes and no more. A question about the
        // rest of the pin, to share its answer with the pieces after, would
        // scan what is left of the pin once for every piece when the pin is not
        // one run: work that grows with the square of what the caller chose.
        Address run{};
        if (auto const status = run_at_locked(pin, start, length, run); status != Status::Ok) {
            return status;
        }
        if (run.size < length) {
            return Status::InternalError;
        }
        out[at] = PhysAddr{run.address};
    }
    return Status::Ok;
}

auto PmtDispatcher::unpin() -> void {
    m_canary.verify<Self>();

    BtiDispatcher::Pin *pin = nullptr;
    {
        Guard guard{get_lock()};
        pin = std::exchange(m_pin, nullptr);
    }
    if (pin != nullptr) {
        m_bti->release_active(*pin);
    }
}

auto PmtDispatcher::on_zero_handles() -> void {
    BtiDispatcher::Pin *pin = nullptr;
    {
        Guard guard{get_lock()};
        pin = std::exchange(m_pin, nullptr);
    }
    if (pin != nullptr) {
        m_bti->quarantine(*pin);
    }
}
} // namespace ours::object
