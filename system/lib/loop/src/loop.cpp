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
    // Nothing is running, so no callback is left to close these. Each close
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

auto Loop::add(ours::Uandle const handle, ours::Signals const signals, Callback const callback,
               void *const context) -> ustl::Result<Key, ours::Status> {
    if (handle == ours::Uandle::Invalid || signals == ours::Signals::None || callback == nullptr) {
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

    auto &slot = m_slots[index];
    slot.m_handle = handle;
    slot.m_signals = signals;
    slot.m_callback = callback;
    slot.m_context = context;
    if (auto const status = arm(index); status != ours::Status::Ok) {
        // Nothing was armed, so no packet can carry this key: the slot goes
        // back free in the same life, and the handle stays the caller's.
        slot.m_handle = ours::Uandle::Invalid;
        slot.m_signals = ours::Signals::None;
        slot.m_callback = nullptr;
        slot.m_context = nullptr;
        return ustl::err(status);
    }
    return ustl::ok(Key{index, slot.m_generation});
}

auto Loop::arm(u32 const index) -> ours::Status {
    auto const &slot = m_slots[index];
    return ours::object_wait_async(slot.m_handle, m_port, Key{index, slot.m_generation}.raw(),
                                   slot.m_signals, ours::WaitAsyncOptions::None);
}

// The spec is the comment on Loop::run() in <loop/loop.hpp>. Until the user
// writes this body, run() dispatches nothing and answers NotSupported, which
// every test of it in test/loop_test.cpp refuses.
auto Loop::run([[maybe_unused]] ours::Instant const deadline) -> ours::Status {
    // Written by the user (P3): see the spec above.
    return ours::Status::NotSupported;
}
} // namespace loop
