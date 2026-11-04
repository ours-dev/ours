/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOOP_LOOP_HPP
#define LOOP_LOOP_HPP 1

#include <span>

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/util/enum_bits.hpp>

/// One port and a fixed table of wait slots, through which a single-threaded
/// server waits on many handles at once: the root's every wait
/// (docs/design/261004/component-framework.md, "Concurrency: one thread and
/// one Loop"), and a provider's, through ioservice::Outgoing
/// (examples/echo). It does waits and nothing else: no tasks, no timers, no
/// threads. Each wait lives in a slot of a table the caller sizes, not in a
/// wait object the caller allocates, because a user program has no heap.
///
/// The objects:
///   - the port, which every wait delivers its packet to;
///   - the slots, each {handle, signals, waiter, generation, flags};
///   - the Waiter each slot in use points at, which the caller owns;
///   - one one-shot, level-triggered wait per slot in use that is not
///     suspended (object_wait_async), which queues one SignalOne packet
///     carrying the slot's key once the handle asserts any of the slot's
///     signals.
///
/// The events: add() takes a free slot and arms its wait; run() takes a
/// packet, finds its slot by the key and calls the slot's Waiter, which
/// handles one message and answers Rearm, Close or Suspend; run() then arms
/// the wait again, closes the handle and frees the slot, or leaves the slot
/// in use with nothing armed until someone calls resume() with its key, which
/// arms the wait. When it could not arm the wait again, either of them also
/// tells the Waiter, once the slot is free.
namespace loop {
/// What a Waiter asks the Loop to do with its slot once on_signals() returns.
enum class Action : u8 {
    /// Wait again for the same signals, under the same key.
    Rearm,

    /// Close the slot's handle and free the slot.
    Close,

    /// Leave the slot in use and arm nothing: the handle stays open, the
    /// generation stays, the key stays valid. Nothing calls the Waiter again
    /// until Loop::resume(key) arms the wait, and what the handle asserts in
    /// the meantime waits in the object (a channel keeps its messages and
    /// its closed peer), not on the port. See Waiter.
    Suspend,
};

/// What a slot waits on: an object whose on_signals() the Loop calls when the
/// slot's wait fires. Whatever a wait serves -- a connection, a client, one of
/// an instance's two waits -- is a Waiter, and the Loop calls it directly.
/// |observed| is what the packet's object asserted when the packet was
/// queued (PacketSignal::observed).
///
/// on_signals() handles one unit of work -- one message read, one exit
/// reported -- and answers. It never closes its own handle (it answers Close
/// instead), never closes another slot's handle, and never arms a wait (it
/// answers Rearm, or Suspend and leaves the arming to resume()); it may add()
/// new slots, and resume() the suspended slot of another Waiter.
///
/// It answers Rearm only once it has consumed what asserted the signal. The
/// wait is level-triggered, so a Waiter that answers Rearm with its message
/// still unread, or with only PeerClosed or Terminated asserted, has its
/// packet queued again at once: run() calls it again on every turn, without
/// end, and run(0) never finds the port empty. That is a spin, not a stall
/// (the packet queues behind everyone else's, I4), but it is the one thread's
/// CPU.
///
/// It answers Suspend when the unit of work it took is not finished and the
/// next one must not be handed to it before it is: a connection that took a
/// request whose reply waits on another party. run() marks the slot suspended
/// and does nothing else. No wait is armed, so no packet is queued, nothing
/// calls the Waiter again, and no signal reaches it, PeerClosed included. The
/// handle stays open and the slot stays in use, so add() never takes it and
/// its key stays valid. Whatever the handle asserts meanwhile stays in the
/// object, and Suspend does not spin over an unread message as Rearm does,
/// since nothing is waiting for it. The Waiter, or whoever knows the slot's
/// key, calls Loop::resume(key) once the Waiter is ready for the next unit of
/// work; the wait is armed then, with the same key and signals, and a message
/// or a closed peer that arrived meanwhile queues its packet at once (the
/// wait is level-triggered), behind the packets already queued (I4). It sees
/// the closed peer then, as PeerClosed alone if nothing else was queued, and
/// answers Close as for any closed peer. resume() from inside the on_signals()
/// that is about to answer Suspend is BadState: the slot is marked only after
/// on_signals() returns, so it is not yet suspended. A Waiter that answers
/// Suspend and is never resumed keeps its slot and its handle for good, and
/// the Loop does not tell it: Suspend frees nothing, so there is nothing to
/// let go of. The Loop's destructor closes the handle of a suspended slot as
/// of any other.
///
/// on_rearm_failed() is the one other thing the Loop tells a Waiter. A Waiter
/// that answered Rearm is not told whether the wait was armed again: arming
/// is the Loop's (a Waiter never arms its next wait itself), and it can fail.
/// The same goes for the wait resume() arms for a slot that answered Suspend.
/// object_wait_async answers NoMemory when the kernel has no memory for the
/// wait, and, in theory, NoResource, BadType, AccessDenied and InvArgs; it
/// answers BadHandle when a Waiter broke the rules above and closed or sent
/// away its own handle and still answered Rearm. The Loop then frees the slot
/// as it does for Close, and the Waiter's owner is the only one who knows
/// what it keeps for a slot -- an entry of a connection table, a client's
/// state -- and so the only one who can let it go. Without the call it would
/// keep it for ever, and a table of such entries would refuse everyone after
/// enough failures.
///
/// The Loop calls on_rearm_failed() once, with the status arm() answered as
/// |cause|, after it has closed the slot's handle, bumped the slot's
/// generation and freed the slot. run() calls it after an answer of Rearm, and
/// resume() before it returns, after an answer of Suspend; the Waiter cannot
/// tell the two apart, and need not. So the Waiter must not touch the handle,
/// which is closed and whose number may name another object by now, and must
/// release whatever it holds for the slot, the way it does when it answers
/// Close. It may add() new slots, this one's among them: the slot is free,
/// and a wait armed in it has the new generation. Like on_signals(), it never
/// waits and never calls run().
///
/// It is not called after the answer Close, whose release the Waiter has
/// done itself; not after the answer Suspend, which frees nothing; not when
/// add() fails, whose caller has the status and keeps the handle; not by a
/// resume() unless the kernel refused its arm; and not by the Loop's
/// destructor, which runs only when the Waiters' owners are going too. Its
/// default does nothing, which is right for a Waiter that only ever answers
/// Close, and so is never told.
///
/// The Loop does not own a Waiter: add() keeps its address, and nothing
/// copies, moves or deletes it. It must outlive its slot, which is to say
/// that it stays where it is from add() until the slot is freed -- by its
/// Close, by a re-arm that failed, or by the Loop's destructor -- and, after a
/// re-arm that failed, until on_rearm_failed() returns. A suspended slot is
/// in use: its Waiter stays where it is for as long as the slot is suspended.
/// The Loop reaches it only through on_signals(), and only for a packet that
/// names its slot in use and not suspended (step 1 of run()), and through
/// on_rearm_failed(), once, right after it frees the slot; once the slot is
/// freed no packet names it, so the Waiter may go then.
///
/// Nothing is deleted through a Waiter, so its destructor is not virtual: a
/// virtual one would add a deleting destructor, which a program with no heap
/// has no use for. Its constructor is constexpr, so that a derived class can
/// be constant-initialised. The second virtual function costs a slot in the
/// vtable of each Waiter class, and nothing in each object.
class Waiter {
  public:
    virtual auto on_signals(ours::Signals observed) -> Action = 0;

    virtual auto on_rearm_failed([[maybe_unused]] ours::Status const cause) -> void {}

  protected:
    constexpr Waiter() = default;
    constexpr Waiter(Waiter const &) = default;
    constexpr auto operator=(Waiter const &) -> Waiter & = default;
    constexpr ~Waiter() = default;
};

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

    /// What is true of the slot beyond what its other fields say. Nothing
    /// else is stored for it, so a free slot has none.
    enum class Flags : u8 {
        None = 0,

        /// Its Waiter answered Suspend and the slot has not been resumed: in
        /// use, with no wait armed and no packet queued. run() sets it once
        /// on_signals() has returned, which is why a resume() from inside
        /// that on_signals() finds it clear; resume() clears it, and so does
        /// freeing the slot.
        Suspended = 1U << 0,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);

    /// The handle the slot waits on, which the Loop owns while the slot is in
    /// use, suspended or not. Invalid while the slot is free: that is what
    /// "free" means.
    ours::Uandle m_handle = ours::Uandle::Invalid;

    /// The signals its wait asks for, the same on every re-arm.
    ours::Signals m_signals = ours::Signals::None;

    /// What run() calls when the slot's wait fires: the Waiter add() was
    /// given, which the caller owns. Null while the slot is free.
    Waiter *m_waiter = nullptr;

    /// The slot's life, the high half of its key. Bumped each time the slot
    /// is freed, so that no key handed out before matches it again. It wraps
    /// after 2^32 lives, which no table lives long enough to see twice with a
    /// packet still queued.
    u32 m_generation = 0;

    /// Sits in the 4 bytes of padding that follow the generation, so a slot
    /// is still 24 bytes (test/loop_test.cpp asserts it).
    Flags m_flags = Flags::None;
};

/// The port and the table; see the namespace's comment.
class Loop final {
  public:
    /// Takes |port|, a port this process made, and |slots|, the table, which
    /// must outlive the Loop. Every slot is made free, at generation 0.
    ///
    /// |slots| holds at most PORT_MAX_OBSERVERS entries, which the caller
    /// checks with a static_assert where it sizes them: each slot in use has
    /// at most one live wait on the port, armed or with its packet queued
    /// (none while it is suspended, and then one again once resumed), so a
    /// larger table could meet NoResource from the port's observer limit
    /// (<ours/port.hpp>).
    ///
    /// The port is the Loop's from here on, and the Loop never hands it out.
    /// The caller may still hold a duplicate; only a check that forges
    /// packets has reason to, and it forges only keys that name no slot in
    /// use. run() drops every User packet, but a duplicate with Write can
    /// also arm a wait of its own under any key, and the SignalOne packet
    /// that wait queues cannot be told from the Loop's own.
    Loop(ours::Uandle port, std::span<Slot> slots);

    /// Closes the handle of every slot in use, suspended ones included, then
    /// the port. Only for a Loop that is not running: the root's never ends.
    ~Loop();

    Loop(Loop const &) = delete;
    auto operator=(Loop const &) -> Loop & = delete;
    Loop(Loop &&) = delete;
    auto operator=(Loop &&) -> Loop & = delete;

    /// Waits on |handle| for any of |signals|: takes the lowest free slot,
    /// fills it in, and arms its wait. When the wait fires, run() calls
    /// waiter.on_signals() with the signals the packet observed. |waiter|
    /// must outlive the slot; see Waiter.
    ///
    /// On success the handle is the Loop's: only the slot's own Waiter, by
    /// answering Close, gets it closed. The key is the slot's index and
    /// generation; the caller needs it only to recognise the slot.
    ///
    /// On failure nothing changed: the slot stays free, and the handle stays
    /// the caller's, who may still send an epitaph on it before closing it.
    ///   - InvArgs: |handle| is Invalid or |signals| is None.
    ///   - NoResource: every slot is in use.
    ///   - otherwise object_wait_async's answer: BadHandle, AccessDenied
    ///     without the Wait right, and so on.
    ///
    /// A Waiter may call add() from on_signals(): the slot whose Waiter is
    /// running is in use, so it is never the one taken. Nor is a suspended
    /// slot: it is in use, with its handle open, until its Waiter is resumed
    /// and answers Close.
    [[nodiscard]]
    auto add(ours::Uandle handle, ours::Signals signals, Waiter &waiter)
        -> ustl::Result<Key, ours::Status>;

    /// Arms the wait of the slot |key| names, which its Waiter left suspended
    /// (Action::Suspend): the same handle, signals and key as before. The
    /// packet reaches the Waiter like any other, behind the port's queue
    /// (I4); if the handle already asserts one of the signals -- a message
    /// that arrived, the peer that went -- the wait is level-triggered, so the
    /// packet is queued at once, and a peer that closed meanwhile is seen now,
    /// as PeerClosed alone if nothing else was left to read.
    ///
    /// It answers by the state of the slot at |key|'s index:
    ///   - Ok: the slot was suspended in the key's life; it is armed.
    ///   - BadState: the slot is in use in the key's life but is not
    ///     suspended -- its wait is armed or its packet queued, which is also
    ///     what a second resume() finds -- or its Waiter is running: the slot
    ///     is marked suspended only after on_signals() returns, so a resume()
    ///     called from inside the on_signals() that is about to answer
    ///     Suspend finds it unmarked. Nothing changes.
    ///   - NotFound: |key| names no slot in use in its life: an index past the
    ///     table, a free slot, or a generation other than the slot's current
    ///     one, which is a key handed out before the slot was closed. A
    ///     slot's next life is untouched by the key of an earlier one.
    ///     Nothing changes. (A second resume() answers BadState while the
    ///     slot is still armed, and NotFound once its Waiter has answered
    ///     Close and the slot is free.)
    ///   - otherwise object_wait_async's answer, when the arm is refused
    ///     (NoMemory, in practice): the slot is freed as a re-arm that failed
    ///     frees it in step 4 of run() -- the handle closed, the generation
    ///     bumped -- and the Waiter's on_rearm_failed() is called once, with
    ///     the answer, before resume() returns. The Waiter releases what it
    ///     kept for the slot there, so the caller of resume() finds the key
    ///     stale and the slot's state gone when it returns.
    ///
    /// Like add(), resume() makes object_wait_async and, on that failure,
    /// handle_close, and nothing else. It is called on the thread that runs
    /// run(): from a Waiter's on_signals() or on_rearm_failed() -- another
    /// slot's, never its own, whose slot is not suspended yet -- or between
    /// calls to run().
    [[nodiscard]]
    auto resume(Key key) -> ours::Status;

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
    ///      key names a slot in use in its current life, and not suspended:
    ///      Key::from_raw() of the packet's key has an index below the
    ///      table's size, the slot at that index is in use (its handle is not
    ///      Invalid; see Slot), its generation is the key's, and it is not
    ///      marked suspended. A packet that fails any of these is no wait of
    ///      this Loop's: a slot freed and taken again since its wait was
    ///      armed, a free slot, an index out of range, a suspended slot (no
    ///      wait is armed for one, so no packet of the Loop's can name it), or
    ///      a User packet that someone holding a duplicate of the port queued.
    ///      The status of a SignalOne packet is always Ok (<ours/port.hpp>),
    ///      so it is not looked at.
    ///   2. Call the slot's Waiter's on_signals() once, with the packet's
    ///      observed signals. The slot stays in use while it runs, and is not
    ///      marked suspended, whatever the Waiter is about to answer.
    ///   3. If on_signals() answered Rearm, arm the slot's wait again with
    ///      arm(): the same key, the same signals. If it answered Suspend,
    ///      mark the slot suspended and go on to the next packet: arm
    ///      nothing, close nothing, leave the generation, the handle and the
    ///      Waiter as they are. resume() arms the wait later.
    ///   4. If it answered Close, or if arm() failed in step 3: close the
    ///      slot's handle, bump the slot's generation and free the slot (its
    ///      handle Invalid, so that add() may take it). Nothing else is
    ///      closed, and nothing is armed for it. If it was arm() that failed,
    ///      then, with the slot already free, call the slot's Waiter's
    ///      on_rearm_failed() once, with the status arm() answered. The Waiter
    ///      and the status are taken before the slot is cleared. run() touches
    ///      the slot no more after the call: an add() from it may have taken
    ///      the slot again, in its new life.
    ///
    /// Invariants it keeps (the design's "Concurrency" section):
    ///   - I1: it blocks only in port_wait, and makes no system call but
    ///     port_wait, object_wait_async (through arm()) and handle_close;
    ///     on_rearm_failed() adds none of its own. The host test links a fake
    ///     of exactly those three. resume() makes the same two and no more.
    ///   - I3: a slot's handle is closed only in step 4, for that slot's own
    ///     Waiter's answer, or for the failure of the arm that answer asked
    ///     for (the re-arm of step 3, or the resume() of a Suspend), and by
    ///     the destructor. When on_signals() runs, the slot's only wait has
    ///     fired and its packet has been taken, so closing the handle there
    ///     leaves nothing of the slot on the port; a suspended slot has no
    ///     wait and no packet either, so the same holds for the arm that
    ///     resume() could not make. The generation check of step 1 is a
    ///     defence, for packets no wait of this Loop's queued.
    ///   - I4: one on_signals() per packet, then re-arm, close or suspend. A
    ///     client with more messages queued is not called again at once: its
    ///     level-triggered wait queues a fresh packet behind everyone else's,
    ///     so a busy client is served in turns. A suspended client is not
    ///     called at all until resumed, and then its packet queues behind
    ///     everyone else's as well.
    ///   - At most one live wait per slot in use, armed or with its packet
    ///     queued, and none for a free slot or a suspended one.
    ///
    /// Why a key carries a generation: ours has no port_cancel yet, so a
    /// packet already queued cannot be withdrawn. A key that named only where
    /// the wait lives -- a slot's index or address -- would hand a stale
    /// packet, one queued for a slot freed and taken again since, to the
    /// slot's new Waiter; a key that was the address of a wait object the
    /// caller frees would make the same packet a use-after-free. A stale key
    /// only fails step 1's check, and the packet is dropped; the same check in
    /// resume() answers NotFound to it. And a Waiter never arms its next wait
    /// itself: its answer tells run() to (step 3), or resume() to after a
    /// Suspend, so add() arms a slot's first wait, run() every later one after
    /// a Rearm, and resume() the one after a Suspend.
    [[nodiscard]]
    auto run(ours::Instant deadline) -> ours::Status;

  private:
    /// Arms the wait of the slot at |index|, which is in use: on its handle,
    /// for its signals, with its current key, delivering to the port.
    /// object_wait_async's answer; on failure nothing is armed.
    [[nodiscard]]
    auto arm(u32 index) -> ours::Status;

    /// Frees the slot at |index|, which is in use and has no live wait: its
    /// wait fired and its packet was taken, or it was suspended. Closes the
    /// handle, clears the fields, bumps the generation, and answers the
    /// Waiter it had, so that a caller that must tell it can. It is step 4 of
    /// run().
    auto free_slot(u32 index) -> Waiter &;

    /// The arm for the slot at |index| was refused with |cause|: frees the
    /// slot and tells its Waiter, once, with on_rearm_failed(). The slot is
    /// cleared first, since the call may add() and so take this very slot
    /// again; the caller touches it no more.
    auto fail_arm(u32 index, ours::Status cause) -> void;

    ours::Uandle m_port;
    std::span<Slot> m_slots;
};
} // namespace loop

#endif // #ifndef LOOP_LOOP_HPP
