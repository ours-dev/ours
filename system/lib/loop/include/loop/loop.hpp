/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOOP_LOOP_HPP
#define LOOP_LOOP_HPP 1

#include <span>

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// One port and a fixed table of wait slots, through which a single-threaded
/// server waits on many handles at once: the root's every wait
/// (docs/design/261004/component-framework.md, "Concurrency: one thread and
/// one Loop"), and later ioservice::Outgoing's. It does waits and nothing else: no
/// tasks, no timers, no threads. Each wait lives in a slot of a table the
/// caller sizes, not in a wait object the caller allocates, because a user
/// program has no heap.
///
/// The objects:
///   - the port, which every wait delivers its packet to;
///   - the slots, each {handle, signals, callback, context, generation};
///   - one one-shot, level-triggered wait per slot in use (object_wait_async),
///     which queues one SignalOne packet carrying the slot's key once the
///     handle asserts any of the slot's signals.
///
/// The events: add() takes a free slot and arms its wait; run() takes a
/// packet, finds its slot by the key and calls the callback, which handles
/// one message and answers Rearm or Close; run() then arms the wait again or
/// closes the handle and frees the slot.
namespace loop {
/// What a callback asks the Loop to do with its slot once it returns.
enum class Action : u8 {
    /// Wait again for the same signals, under the same key.
    Rearm,

    /// Close the slot's handle and free the slot.
    Close,
};

/// A slot's callback: a plain function and the context add() was given,
/// never a virtual call. |observed| is what the packet's object asserted when
/// the packet was queued (PacketSignal::observed).
///
/// A callback handles one unit of work -- one message read, one exit
/// reported -- and answers. It never closes its own handle (it answers Close
/// instead), never closes another slot's handle, and never arms a wait; it
/// may add() new slots.
///
/// It answers Rearm only once it has consumed what asserted the signal. The
/// wait is level-triggered, so a callback that answers Rearm with its message
/// still unread, or with only PeerClosed or Terminated asserted, has its
/// packet queued again at once: run() calls it again on every turn, without
/// end, and run(0) never finds the port empty. That is a spin, not a stall
/// (the packet queues behind everyone else's, I4), but it is the one thread's
/// CPU.
using Callback = auto (*)(void *context, ours::Signals observed) -> Action;

/// The key of a slot's waits: its generation in the high 32 bits and its
/// index in the low 32 (FS:311, docs/design/260924/userspace-filesystem.md).
/// A packet's key names the slot and the slot's life it was armed in: a slot
/// freed and taken again has a new generation, so a packet from its earlier
/// life no longer matches.
class Key final {
  public:
    constexpr Key(u32 const index, u32 const generation)
        : m_raw{(static_cast<u64>(generation) << 32) | index} {}

    /// The key a packet carries, taken back apart. Any u64 is some key; it is
    /// run()'s job to find out whether it names a slot in use.
    static constexpr auto from_raw(u64 const raw) -> Key {
        return Key{static_cast<u32>(raw), static_cast<u32>(raw >> 32)};
    }

    [[nodiscard]]
    constexpr auto index() const -> u32 {
        return static_cast<u32>(m_raw);
    }

    [[nodiscard]]
    constexpr auto generation() const -> u32 {
        return static_cast<u32>(m_raw >> 32);
    }

    /// The value object_wait_async is given and PortPacket::key hands back.
    [[nodiscard]]
    constexpr auto raw() const -> u64 {
        return m_raw;
    }

    friend constexpr auto operator==(Key, Key) -> bool = default;

  private:
    u64 m_raw;
};

class Loop;

/// One entry of the table a Loop waits through. The caller provides the
/// storage, sized at build time (FS §4.2), and never touches a slot: its
/// fields are the Loop's.
class Slot final {
  public:
    constexpr Slot() = default;

  private:
    friend class Loop;

    /// The handle the slot waits on, which the Loop owns while the slot is in
    /// use. Invalid while the slot is free: that is what "free" means.
    ours::Uandle m_handle = ours::Uandle::Invalid;

    /// The signals its wait asks for, the same on every re-arm.
    ours::Signals m_signals = ours::Signals::None;

    Callback m_callback = nullptr;
    void *m_context = nullptr;

    /// The slot's life, the high half of its key. Bumped each time the slot
    /// is freed, so that no key handed out before matches it again. It wraps
    /// after 2^32 lives, which no table lives long enough to see twice with a
    /// packet still queued.
    u32 m_generation = 0;
};

/// The port and the table; see the namespace's comment.
class Loop final {
  public:
    /// Takes |port|, a port this process made, and |slots|, the table, which
    /// must outlive the Loop. Every slot is made free, at generation 0.
    ///
    /// |slots| holds at most PORT_MAX_OBSERVERS entries, which the caller
    /// checks with a static_assert where it sizes them: each slot in use has
    /// exactly one live wait on the port, armed or with its packet queued,
    /// so a larger table could meet NoResource from the port's observer limit
    /// (<ours/port.hpp>).
    ///
    /// The port is the Loop's from here on, and the Loop never hands it out.
    /// The caller may still hold a duplicate; only a check that forges
    /// packets has reason to, and it forges only keys that name no slot in
    /// use. run() drops every User packet, but a duplicate with Write can
    /// also arm a wait of its own under any key, and the SignalOne packet
    /// that wait queues cannot be told from the Loop's own.
    Loop(ours::Uandle port, std::span<Slot> slots);

    /// Closes the handle of every slot in use, then the port. Only for a Loop
    /// that is not running: the root's never ends.
    ~Loop();

    Loop(Loop const &) = delete;
    auto operator=(Loop const &) -> Loop & = delete;
    Loop(Loop &&) = delete;
    auto operator=(Loop &&) -> Loop & = delete;

    /// Waits on |handle| for any of |signals|: takes the lowest free slot,
    /// fills it in, and arms its wait. When the wait fires, run() calls
    /// |callback| with |context|.
    ///
    /// On success the handle is the Loop's: only the slot's own callback,
    /// by answering Close, gets it closed. The key is the slot's index and
    /// generation; the caller needs it only to recognise the slot.
    ///
    /// On failure nothing changed: the slot stays free, and the handle stays
    /// the caller's, who may still send an epitaph on it before closing it.
    ///   - InvArgs: |handle| is Invalid, |signals| is None, or |callback| is
    ///     null.
    ///   - NoResource: every slot is in use.
    ///   - otherwise object_wait_async's answer: BadHandle, AccessDenied
    ///     without the Wait right, and so on.
    ///
    /// A callback may call add(): the slot whose callback is running is in
    /// use, so it is never the one taken.
    [[nodiscard]]
    auto add(ours::Uandle handle, ours::Signals signals, Callback callback, void *context)
        -> ustl::Result<Key, ours::Status>;

    /// Dispatches packets until port_wait answers anything but Ok, and
    /// returns that answer. This is the Loop's dispatch, which P3 in
    /// .agents/tasks/component-framework/PLAN.md reserved for the user to
    /// write by hand; the AI wrote it for the MVP on the user's instruction of
    /// 2026-10-04, and the exercise stays open. This comment is its whole
    /// spec, and test/loop_test.cpp and the shell's check_loop() pin it down.
    ///
    /// Input. |deadline| is handed to every port_wait unchanged:
    /// INSTANT_INFINITE for a server that never stops, as the root, or 0 to
    /// dispatch whatever is queued and return once the port is empty, as the
    /// checks do. Any other value is port_wait's to refuse (NotSupported).
    ///
    /// Output. The first answer from port_wait other than Ok:
    ///   - TimeOut: the port was empty under deadline 0, the normal way out of
    ///     run(0);
    ///   - BadHandle, BadType or AccessDenied: the port is not a port this
    ///     process can read; NotSupported: a deadline port_wait does not take;
    ///     InvArgs: the packet could not be written.
    /// run() never returns Ok, and has no error of its own: whatever goes
    /// wrong inside one dispatch is dealt with there, and the loop goes on.
    ///
    /// For each packet port_wait hands out, in this order:
    ///   1. Drop it, calling nothing, unless it is a SignalOne packet whose
    ///      key names a slot in use in its current life: Key::from_raw() of
    ///      the packet's key has an index below the table's size, the slot
    ///      at that index is in use (its handle is not Invalid; see Slot),
    ///      and its generation is the key's. A
    ///      packet that fails any of these is no wait of this Loop's: a slot
    ///      freed and taken again since its wait was armed, a free slot, an
    ///      index out of range, or a User packet that someone holding a
    ///      duplicate of the port queued. The status of a SignalOne packet is
    ///      always Ok (<ours/port.hpp>), so it is not looked at.
    ///   2. Call the slot's callback once, with the slot's context and the
    ///      packet's observed signals. The slot stays in use while the
    ///      callback runs.
    ///   3. If the callback answered Rearm, arm the slot's wait again with
    ///      arm(): the same key, the same signals.
    ///   4. If it answered Close, or if arm() failed in step 3: close the
    ///      slot's handle, bump the slot's generation and free the slot (its
    ///      handle Invalid, so that add() may take it). Nothing else is
    ///      closed, and nothing is armed for it.
    ///
    /// Invariants it keeps (the design's "Concurrency" section):
    ///   - I1: it blocks only in port_wait, and makes no system call but
    ///     port_wait, object_wait_async (through arm()) and handle_close. The
    ///     host test links a fake of exactly those three.
    ///   - I3: a slot's handle is closed only in step 4, for that slot's own
    ///     callback's answer. When the callback runs, the slot's only wait has
    ///     fired and its packet has been taken, so closing the handle there
    ///     leaves nothing of the slot on the port; the generation check of
    ///     step 1 is a defence, for packets no wait of this Loop's queued.
    ///   - I4: one callback per packet, then re-arm. A client with more
    ///     messages queued is not called again at once: its level-triggered
    ///     wait queues a fresh packet behind everyone else's, so a busy client
    ///     is served in turns.
    ///   - At most one live wait per slot in use, armed or with its packet
    ///     queued, and none for a free slot.
    ///
    /// Why a key carries a generation: ours has no port_cancel yet, so a
    /// packet already queued cannot be withdrawn. A key that named only where
    /// the wait lives -- a slot's index or address -- would hand a stale
    /// packet, one queued for a slot freed and taken again since, to the
    /// slot's new callback; a key that was the address of a wait object the
    /// caller frees would make the same packet a use-after-free. A stale key
    /// only fails step 1's check, and the packet is dropped. And a callback
    /// never arms its next wait itself: its answer tells run() to (step 3),
    /// so add() arms a slot's first wait and run() every later one.
    [[nodiscard]]
    auto run(ours::Instant deadline) -> ours::Status;

  private:
    /// Arms the wait of the slot at |index|, which is in use: on its handle,
    /// for its signals, with its current key, delivering to the port.
    /// object_wait_async's answer; on failure nothing is armed.
    [[nodiscard]]
    auto arm(u32 index) -> ours::Status;

    ours::Uandle m_port;
    std::span<Slot> m_slots;
};
} // namespace loop

#endif // #ifndef LOOP_LOOP_HPP
