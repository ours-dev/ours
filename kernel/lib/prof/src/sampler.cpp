/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prof/format.hpp>
#include <prof/sampler.hpp>

#include <algorithm>
#include <atomic>

#include <ours/status.hpp>
#include <unwind/unwinder.hpp>

namespace prof {
namespace {
    /// Stack window handed to the unwinder's memory reader: samples only
    /// read slots from the interrupted thread's kernel stack, so a corrupt
    /// frame pointer cannot turn into a wild kernel read.
    constexpr u64 STACK_WINDOW = 0x1'0000;

    static_assert(STACK_WINDOW >= sizeof(u64),
                  "the window must hold at least one slot, or hi - sizeof(u64) underflows");

    struct StackWindow final {
        u64 lo;
        u64 hi;

        /// Builds the window above `sp`, saturating at the top of the address
        /// space.
        ///
        /// `sp + STACK_WINDOW` is not safe to write plainly: a stack pointer
        /// near the top would wrap the end to a low address, and every bounds
        /// check against it afterwards would pass. `sp` comes out of an
        /// interrupted CPU's trap frame, so it is exactly the kind of value
        /// this window exists to distrust.
        [[nodiscard]]
        static auto above(u64 sp) -> StackWindow {
            u64 const room = ~u64{0} - sp;
            return StackWindow{
                .lo = sp,
                .hi = room < STACK_WINDOW ? ~u64{0} : sp + STACK_WINDOW,
            };
        }
    };

    /// Reads one word of the interrupted thread's stack, refusing anything
    /// outside the window.
    ///
    /// The comparison is deliberately written as `address > hi - sizeof(u64)`
    /// rather than `hi < address + sizeof(u64)`. `address` is an unwound frame
    /// pointer, i.e. the one input here that may be arbitrary: near the top of
    /// the address space `address + 8` wraps to a small number, `hi` compares
    /// greater, and the check the comment above this window promised -- that a
    /// corrupt frame pointer cannot become a wild kernel read -- silently
    /// stopped holding. `hi - sizeof(u64)` cannot underflow because the window
    /// is at least that wide (static_assert above) and `above()` never wraps.
    ///
    /// Alignment is checked for the same reason. x86 tolerates an unaligned
    /// load, but this library is built for arm and riscv too, where the same
    /// corrupt pointer would fault inside an interrupt handler instead of
    /// being rejected.
    auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (address < window->lo || address > window->hi - sizeof(u64)) {
            return false;
        }
        if ((address % alignof(u64)) != 0) {
            return false;
        }
        *out = *reinterpret_cast<u64 const *>(address);
        return true;
    }
} // namespace

auto Sampler::unwind_context(SampleContext const &context, SampleRecord *record) const -> void {
    unwind::Registers regs{
        .pc = context.pc,
        .sp = context.sp,
        .bp = context.bp,
    };
    auto window = StackWindow::above(context.sp);
    unwind::MemoryReader const reader{&window, &kernel_stack_read};
    unwind::Unwinder const unwinder(&m_eh_section, 1, &m_fde_index);

    while (record->depth < MAX_STACK_DEPTH) {
        auto const step = unwinder.step(&regs, reader);
        if (step != unwind::StepResult::Ok) {
            // NoFde is the natural leaf stop; anything else is a prefix.
            if (step != unwind::StepResult::NoFde) {
                record->flags |= RecordFlags::Truncated;
            }
            break;
        }
        record->ips[record->depth] = regs.pc;
        ++record->depth;
    }
    if (record->depth == MAX_STACK_DEPTH) {
        record->flags |= RecordFlags::Truncated;
    }
}

auto Sampler::init(Resources const &resources) -> InitFlags {
    auto const buffers_ready = m_sink.init(Sink::Resources{
        .buffer_lookup = resources.buffer_lookup,
        .buffer_lookup_context = resources.buffer_lookup_context,
        .num_cpus = resources.num_cpus,
    });

    m_eh_section = resources.eh_section;
    m_fde_index.init(resources.fde_entries, resources.fde_entry_count);
    m_image_base = resources.image_base;

    // `Sink::init` reports whether a buffer actually resolves, not merely
    // whether a lookup was wired in: the kernel's own CpuLocal allocation
    // can have failed, leaving a valid callback that yields nothing.
    auto flags = InitFlags::None;
    if (buffers_ready) {
        flags = flags | InitFlags::BuffersReady;
    }
    if (resources.fde_entry_count != 0) {
        flags = flags | InitFlags::UnwindReady;
    }
    return flags;
}

auto Sampler::is_running() const -> bool {
    return m_state.load(std::memory_order_acquire) == State::Running;
}

auto Sampler::start() -> ours::Status {
    // Claim the sampler first, in a state no producer acts on. That is what
    // makes the reset below safe to do in the open: no sample can land in a
    // buffer while it is being emptied, because `Starting` is not `Running`.
    auto expected = State::Stopped;
    if (!m_state.compare_exchange_strong(expected, State::Starting, std::memory_order_acq_rel)) {
        return ours::Status::BadState;
    }

    m_sink.reset();

    // The release is the publication point: the reset buffers happen-before
    // any producer that acquires Running.
    m_state.store(State::Running, std::memory_order_release);
    return ours::Status::Ok;
}

auto Sampler::stop() -> void {
    auto expected = State::Running;
    // Stopping a sampler that is not running is a no-op, not an error, so
    // whether the exchange happened does not matter.
    auto _ = m_state.compare_exchange_strong(expected, State::Stopped, std::memory_order_acq_rel);
}

auto Sampler::record_sample(SampleContext const &context) -> void {
    if (m_state.load(std::memory_order_acquire) != State::Running) {
        return;
    }

    auto const cpu = context.cpu;

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.depth = 1;
    record.cpu = static_cast<u16>(cpu);
    record.tid = context.tid;
    record.timestamp = context.timestamp;
    record.ips[0] = context.pc;

    if (context.from_uspace) {
        // The interrupt cannot walk a user stack: reading user memory can
        // fault, and a fault cannot be taken here. A user stack is walked
        // on the thread's way back to user mode and recorded with
        // record_user_stack(); this keeps the pc alone.
        record.flags = RecordFlags::FromUser;
        auto _ = m_sink.produce(cpu, record);
        return;
    }

    unwind_context(context, &record);
    auto _ = m_sink.produce(cpu, record);
}

auto Sampler::record_user_stack(SampleContext const &context, u64 const *const ips,
                                usize const depth) -> void {
    if (m_state.load(std::memory_order_acquire) != State::Running || depth == 0) {
        return;
    }

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.flags = RecordFlags::FromUser;
    record.cpu = static_cast<u16>(context.cpu);
    record.tid = context.tid;
    record.timestamp = context.timestamp;
    auto const kept = std::min(depth, MAX_STACK_DEPTH);
    for (usize i = 0; i < kept; ++i) {
        record.ips[i] = ips[i];
    }
    record.depth = static_cast<u8>(kept);
    auto _ = m_sink.produce(context.cpu, record);
}

auto walk_frame_pointers(u64 const pc, u64 fp, unwind::MemoryReader const &reader, u64 *const ips,
                         usize const max) -> usize {
    if (max == 0) {
        return 0;
    }
    ips[0] = pc;
    usize depth = 1;
    while (depth < max) {
        if (fp == 0 || (fp % alignof(u64)) != 0 || fp > ~u64{0} - 2 * sizeof(u64)) {
            break;
        }
        u64 next = 0;
        u64 ret = 0;
        if (!reader.read(reader.context, fp, &next) ||
            !reader.read(reader.context, fp + sizeof(u64), &ret) || ret == 0) {
            break;
        }
        ips[depth] = ret;
        ++depth;
        // Frames sit at higher addresses the further out they are; one that
        // does not is a corrupt chain, or a loop.
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    return depth;
}

} // namespace prof
