/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <loop/loop.hpp>

#include <memory>

#include <ours/port.hpp>
#include <ours/vdso.hpp>

namespace loop {
Loop::Loop(ours::Uandle const port, std::span<Slot> const slots) : m_port{port}, m_slots{slots} {
    // The caller's storage may have been used before; constructing every
    // slot again is what makes "every slot free, at generation 0" true
    // rather than assumed.
    for (auto &slot: m_slots) {
        std::construct_at(&slot);
    }
}

Loop::~Loop() {
    // Nothing is running, so no Waiter is left to close these. Each close
    // also ends the slot's armed wait; a packet already queued goes with the
    // port, closed last.
    for (auto const &slot: m_slots) {
        if (slot.m_handle != ours::Uandle::Invalid) {
            // The Loop owns the handle, so the close cannot be refused.
            auto _ = ours::handle_close(slot.m_handle);
        }
    }
    // As above: the port is the Loop's.
    auto _ = ours::handle_close(m_port);
}

auto Loop::add(ours::Uandle const handle, ours::Signals const signals, Waiter &waiter)
    -> ustl::Result<Key, ours::Status> {
    if (handle == ours::Uandle::Invalid || signals == ours::Signals::None) {
        return ustl::err(ours::Status::InvArgs);
    }

    // The lowest free slot: a linear scan, which docs/repairs/261004/loop.md
    // records as the cost of every add().
    auto index = u32{0};
    while (index < m_slots.size() && m_slots[index].m_handle != ours::Uandle::Invalid) {
        ++index;
    }
    if (index == m_slots.size()) {
        return ustl::err(ours::Status::NoResource);
    }

    // A free slot has no flags (free_slot() clears them), so the new life
    // starts unsuspended.
    auto &slot = m_slots[index];
    slot.m_handle = handle;
    slot.m_signals = signals;
    slot.m_waiter = &waiter;
    if (auto const status = arm(index); status != ours::Status::Ok) {
        // Nothing was armed, so no packet can carry this key: the slot goes
        // back free in the same life, and the handle stays the caller's.
        slot.m_handle = ours::Uandle::Invalid;
        slot.m_signals = ours::Signals::None;
        slot.m_waiter = nullptr;
        return ustl::err(status);
    }
    return ustl::ok(Key{index, slot.m_generation});
}

auto Loop::resume(Key const key) -> ours::Status {
    // The bound comes before the slot is touched, as in step 1 of run().
    if (key.index() >= m_slots.size()) {
        return ours::Status::NotFound;
    }
    // In use in the key's life, as step 1 of run() reads it. A slot freed
    // since the key was handed out is free or in a later life, whichever
    // flags it has.
    auto &slot = m_slots[key.index()];
    if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation()) {
        return ours::Status::NotFound;
    }
    // Only a slot whose Waiter answered Suspend and returned carries the
    // flag, so this refuses the slot whose Waiter is running (run() sets the
    // flag after on_signals() returns), an armed slot, one whose packet is
    // queued, and a second resume().
    if ((slot.m_flags & Slot::Flags::Suspended) == Slot::Flags::None) {
        return ours::Status::BadState;
    }
    slot.m_flags &= ~Slot::Flags::Suspended;
    // The same arm as step 3 of run(): level-triggered, so a message or a
    // closed peer that arrived while the slot was suspended queues its packet
    // now, behind the ones already queued.
    if (auto const status = arm(key.index()); status != ours::Status::Ok) {
        fail_arm(key.index(), status);
        return status;
    }
    return ours::Status::Ok;
}

auto Loop::arm(u32 const index) -> ours::Status {
    auto const &slot = m_slots[index];
    return ours::object_wait_async(slot.m_handle, m_port, Key{index, slot.m_generation}.raw(),
                                   slot.m_signals, ours::WaitAsyncOptions::None);
}

auto Loop::free_slot(u32 const index) -> Waiter & {
    auto &slot = m_slots[index];
    // The Waiter is kept, since a caller may owe it a call and the slot is
    // cleared before that.
    auto &waiter = *slot.m_waiter;
    // The Loop owns the handle, so the close cannot be refused, unless a
    // Waiter closed it itself and answered Rearm (BadHandle from arm()):
    // there is nothing to do about that either.
    auto _ = ours::handle_close(slot.m_handle);
    slot.m_handle = ours::Uandle::Invalid;
    slot.m_signals = ours::Signals::None;
    slot.m_waiter = nullptr;
    slot.m_flags = Slot::Flags::None;
    // Unsigned, so it wraps after 2^32 lives; see Slot::m_generation.
    ++slot.m_generation;
    return waiter;
}

auto Loop::fail_arm(u32 const index, ours::Status const cause) -> void {
    // The Waiter is told of a failed arm, which only its owner can answer
    // for. The slot is free by then, since the call may add() and so take
    // this very slot again, in its new life: the caller touches it no more.
    free_slot(index).on_rearm_failed(cause);
}

// The spec is the comment on Loop::run() in <loop/loop.hpp>, and the step
// numbers below are its. P3 reserved this body for the user to write by hand;
// the AI wrote it for the MVP on the user's instruction of 2026-10-04, and the
// exercise stays open (docs/changes/261004/loop.md).
auto Loop::run(ours::Instant const deadline) -> ours::Status {
    while (true) {
        auto packet = ours::PortPacket{};
        if (auto const status = ours::port_wait(m_port, deadline, &packet);
            status != ours::Status::Ok) {
            return status;
        }

        // 1. Only a SignalOne packet can be a wait's: port_queue delivers
        //    User whatever its caller wrote, so a User packet is someone
        //    else's, whatever its key says.
        if (packet.type != ours::PacketType::SignalOne) {
            continue;
        }
        // The bound comes before the slot is touched: m_slots[size] is
        // whatever memory follows the table.
        auto const key = Key::from_raw(packet.key);
        if (key.index() >= m_slots.size()) {
            continue;
        }
        // "In use" is the handle, the one field step 4 always clears; the
        // generation then says whether the packet was armed in this life.
        // A suspended slot has no wait armed, so a packet that names one is
        // no wait of this Loop's, whatever its key says.
        auto &slot = m_slots[key.index()];
        if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation() ||
            (slot.m_flags & Slot::Flags::Suspended) != Slot::Flags::None) {
            continue;
        }

        // 2. The slot stays in use while its Waiter runs, so an add() from
        //    inside on_signals() takes another slot, and |slot| still names
        //    this one afterwards: the table is the caller's span and never
        //    moves.
        auto const action = slot.m_waiter->on_signals(packet.signal.observed);

        // 3. One on_signals() per packet, then the wait again (I4). A handle with
        //    more queued is level-triggered, so the new wait's packet goes to
        //    the back of the port's queue, behind every other slot's.
        //    A Suspend arms nothing: the slot is marked, after on_signals()
        //    has returned, and stays as it is until resume().
        if (action == Action::Suspend) {
            slot.m_flags |= Slot::Flags::Suspended;
            continue;
        }
        if (action == Action::Rearm) {
            auto const rearm = arm(key.index());
            if (rearm == ours::Status::Ok) {
                continue;
            }
            // 4, for a re-arm that failed and so left nothing armed: the
            //    slot is freed as for a Close, and its Waiter, which answered
            //    Rearm believing the slot lives on, is told.
            fail_arm(key.index(), rearm);
            continue;
        }

        // 4. Close: the slot has no live wait (its packet was the one just
        //    taken), so closing the handle here leaves nothing of it on the
        //    port (I3). The new generation retires every key of this life.
        //    The Waiter is not told: a Close was its own answer, and it has
        //    released what it held. |slot| is not touched after this call.
        free_slot(key.index());
    }
}
} // namespace loop
