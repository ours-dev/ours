/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_RECORDER_HPP
#define SAMPLER_RECORDER_HPP 1

#include <sampler/buffer.hpp>
#include <sampler/format.hpp>
#include <sampler/stack_window.hpp>

#include <ours/status.hpp>
#include <prof/sink.hpp>
#include <unwind/cfi.hpp>
#include <unwind/fde_index.hpp>
#include <unwind/unwinder.hpp>
#include <ustl/util/enum_bits.hpp>

namespace sampler {

/// One interrupted context, as the kernel hands it to `record_sample()`.
///
/// Every field is a fact the kernel already owns, so the kernel fills them
/// all in: the registers from the interrupt frame it took, the CPU, thread
/// and time from its own state. This library asks nothing of the platform
/// and calls nothing to find these out, which is what keeps it free of an
/// arch seam -- it used to read the CPU and the time through one, and the
/// architecture code that implemented it ended up calling back into
/// `Recorder` from its interrupt handler.
struct SampleContext final {
    u64 pc;
    u64 sp;
    u64 bp;
    u32 tid;
    /// The koid of the interrupted thread's process, stored in the record;
    /// 0 for a kernel thread.
    u64 pid;
    /// Picks the buffer the record goes to, and is stored in it.
    u32 cpu;
    /// Monotonic nanoseconds; used only to order samples for display.
    u64 timestamp;
    /// The stack the interrupted code was running on, [stack_base,
    /// stack_top): the kernel unwind reads only between sp and its top. Both 0
    /// when unknown, and then a kernel stack is not unwound. Not read for a
    /// user sample.
    u64 stack_base;
    u64 stack_top;
    bool from_uspace;
};

/// Where a kernel stack begins: the return address of a call an entry stub
/// makes into C++, and how far below the top of the thread's stack the stub
/// makes it. The unwinder ends a walk at a frame with no unwind information,
/// and only these frames are where a stack really began; anywhere else such
/// a frame is code the unwinder cannot step through, with the rest of the
/// stack below it. Both must match: the same stub can also be entered from
/// kernel code deeper in the stack, where its frame begins nothing.
struct StackRoot final {
    u64 return_address;
    u64 depth;
};

/// One profiling session's recorder: what a sample contains and where it is
/// stored, never when one is taken. The kernel keeps one per session, inside
/// the session that owns its buffers (sampler::Session), rather than
/// one for the whole kernel, so that a session's records can outlive the
/// next session's creation. `Recorder` owns no dynamic storage and
/// makes no calls into `ours::`/`gktl::`/`logz4::`: the per-CPU buffers and
/// the unwind tables come in through `Resources`, each sample's facts come in
/// through `SampleContext`, and the dump goes out through a writer the caller
/// supplies (`ByteWriter` or `prof::LineWriter`).
/// Deciding when a sample is due -- arming the per-CPU timers -- is the
/// Session's (session.hpp), in this library's kernel-only half.
///
/// It has no running state of its own. When it may be written and when read
/// is its owner's to say, since only the owner knows where its producers run:
/// a producer writes only while the owner lets producers in, and reset() and
/// the reads run only while it keeps them out. The Session does both with
/// one atomic, ACTIVE, and the IPI barrier its stop sends.
class Recorder final {
  public:
    using Sink = prof::RecordSink<SampleRecord>;

    /// Looks up the per-CPU sample arena for `cpu`; returns nullptr when
    /// none exists (e.g. `cpu` is out of range). The caller keeps owning
    /// the buffers' actual storage — mirrors `unwind::MemoryReader`'s
    /// context-callback shape.
    using BufferLookup = Sink::BufferLookup;

    /// Everything `init()` needs, all owned and kept alive by the caller.
    /// `Recorder` allocates nothing itself.
    struct Resources final {
        BufferLookup buffer_lookup = nullptr;
        void *buffer_lookup_context = nullptr;
        u32 num_cpus = 0;

        /// The kernel's `.eh_frame`; an empty section (size 0) degrades
        /// unwinding to depth-1 records rather than failing.
        unwind::Section eh_section{};
        /// Ascending-`pc_begin`-sorted FDE entries built by the caller
        /// (mirrors `unwind::FdeIndex::init`'s own storage contract); must
        /// outlive the `Recorder`.
        unwind::FdeIndex::Entry const *fde_entries = nullptr;
        usize fde_entry_count = 0;

        /// Runtime load address of the kernel image, stamped into
        /// `DumpHeader::load_base` for the host tool to subtract.
        u64 image_base = 0;

        /// Where kernel stacks begin; must outlive the `Recorder`. Without
        /// any, every stack that ends at a frame with no unwind information
        /// is marked a prefix.
        StackRoot const *stack_roots = nullptr;
        usize stack_root_count = 0;
    };

    enum class InitFlags : u32 {
        None = 0,
        BuffersReady = 1u << 0,
        UnwindReady = 1u << 1,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(InitFlags);

    Recorder() = default;
    Recorder(Recorder const &) = delete;
    auto operator=(Recorder const &) -> Recorder & = delete;

    /// Wires up `resources` and resets every buffer they reach. Called once,
    /// before the first run; failures are reported through the returned
    /// flags, never fatal.
    auto init(Resources const &resources) -> InitFlags;

    /// Empties every buffer, for a new run. Only while no producer can
    /// write: the owner publishes the emptied buffers to the producers it
    /// lets in afterwards.
    auto reset() -> void;

    /// Take one sample now. Runs in interrupt context: no allocation,
    /// bounded work, unwinds at most `MAX_STACK_DEPTH` frames. Only while
    /// the owner lets producers in, from one producer per CPU at a time.
    auto record_sample(SampleContext const &context) -> void;

    /// Record a user stack the caller has already walked: |ips| from the
    /// interrupted pc outwards, at most `MAX_STACK_DEPTH` of them kept.
    /// |context| gives the CPU, thread and time; its registers are not
    /// looked at. Under the same rule as record_sample(): the Session keeps
    /// interrupts off from its check that the run is on to here, which is
    /// what lets a stop wait it out.
    auto record_user_stack(SampleContext const &context, u64 const *ips, usize depth) -> void;

    /// Counts a tick on |cpu| that the caller records nothing for, because it
    /// landed where no target was asked for; the dump header sums them.
    /// Under the same rule as record_sample().
    auto note_unmatched(u32 cpu) -> void;

    [[nodiscard]]
    auto num_cpus() const -> u32;

    /// Per-CPU buffer statistics; stable once sampling has stopped.
    [[nodiscard]]
    auto buffer_stats(u32 cpu) const -> prof::BufferStats;

    /// Encodes the dump header and every retained record as hex text (the
    /// `PROF1`/`SAMP` line framing documented in `sampler/format.hpp`) and
    /// hands each line to |writer|. Carries no I/O policy of its own.
    auto export_dump(prof::LineWriter const &writer) const -> void;

    /// Where copy_dump() puts the dump's bytes: consecutive pieces of it, at
    /// increasing offsets from 0. A non-Ok answer stops the copy, and
    /// copy_dump() returns it.
    struct ByteWriter final {
        void *context;
        auto (*write)(void *context, usize offset, void const *bytes, usize len) -> ours::Status;
    };

    /// How many bytes copy_dump() writes: one DumpHeader, then every
    /// retained record. Stable once sampling has stopped.
    [[nodiscard]]
    auto dump_size() const -> usize;

    /// The same dump as export_dump(), as raw bytes instead of hex lines:
    /// the header, then every CPU's retained records in CPU order, exactly
    /// dump_size() bytes when it succeeds. For a reader that parses the
    /// structures itself rather than a console that only carries text.
    [[nodiscard]]
    auto copy_dump(ByteWriter const &writer) const -> ours::Status;

  private:
    /// The header both forms of the dump start with.
    [[nodiscard]]
    auto make_header() const -> DumpHeader;

    auto unwind_context(SampleContext const &context, SampleRecord *record) const -> void;

    /// Whether |regs|, a frame with no unwind information, is one of the
    /// roots: its pc a root's return address, at that root's depth below the
    /// top of |context|'s stack.
    [[nodiscard]]
    auto is_stack_root(unwind::Registers const &regs, SampleContext const &context) const -> bool;

    /// Where records land. Everything about *storing* a record lives here
    /// and is shared with any other producer; `Recorder` only decides when a
    /// record happens and what goes in it.
    Sink m_sink;

    unwind::Section m_eh_section{};
    unwind::FdeIndex m_fde_index;
    u64 m_image_base = 0;
    StackRoot const *m_stack_roots = nullptr;
    usize m_stack_root_count = 0;
};

/// Walks a stack by frame pointers, as code built with them lays it out:
/// the word at a frame pointer is the caller's frame pointer, and the word
/// above it the return address into the caller. Writes |pc|, then each
/// return address, into |ips|, at most |max| in all, and returns how many.
///
/// Every read goes through |reader|, so a frame pointer that is wild, or
/// points at memory that is not there, ends the walk rather than faulting.
/// So does a frame that does not move up the stack (the next frame pointer
/// must be above this one, and aligned), which is what keeps a corrupt or
/// cyclic chain from walking forever, and a return address of zero, where
/// a thread's outermost frame stops it.
///
/// A leaf function built without a frame pointer leaves its caller out:
/// the walk starts from the leaf's pc but its caller's frame pointer.
[[nodiscard]]
auto walk_frame_pointers(u64 pc, u64 fp, unwind::MemoryReader const &reader, u64 *ips, usize max)
    -> usize;

/// Reads as "this flag is set". ustl's bitmask operators give `&` and a
/// `!` that answers "no bits at all", so the test is a double negation; the
/// name exists to keep that out of call sites.
[[nodiscard]]
constexpr auto has_flag(Recorder::InitFlags flags, Recorder::InitFlags bit) -> bool {
    return !!(flags & bit);
}

} // namespace sampler

#endif // #ifndef SAMPLER_RECORDER_HPP
