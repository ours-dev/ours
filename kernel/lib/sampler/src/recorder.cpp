/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sampler/format.hpp>
#include <sampler/recorder.hpp>

#include <algorithm>

#include <unwind/unwinder.hpp>

namespace sampler {
namespace {
    /// Reads one word of the interrupted thread's stack for the unwinder,
    /// refusing anything outside the window (StackWindow::contains()).
    auto kernel_stack_read(void *context, u64 address, u64 *out) -> bool {
        auto const *window = static_cast<StackWindow const *>(context);
        if (!window->contains(address)) {
            return false;
        }
        *out = *reinterpret_cast<u64 const *>(address);
        return true;
    }
} // namespace

auto Recorder::unwind_context(SampleContext const &context, SampleRecord *record) const -> void {
    unwind::Registers regs{
        .pc = context.pc,
        .sp = context.sp,
        .bp = context.bp,
    };
    // Not on the stack the kernel named, or no stack named: nothing above
    // the pc can be read safely, so the record keeps the pc alone and says
    // it is a prefix.
    auto window = StackWindow::on_stack(context.sp, context.stack_base, context.stack_top);
    if (window.empty()) {
        record->flags |= RecordFlags::Truncated;
        return;
    }
    unwind::MemoryReader const reader{&window, &kernel_stack_read};
    unwind::Unwinder const unwinder(&m_eh_section, 1, &m_fde_index);

    while (record->depth < MAX_STACK_DEPTH) {
        auto const step = unwinder.step(&regs, reader);
        if (step != unwind::StepResult::Ok) {
            // A frame with no caller (a thread's first frame) ends the stack.
            // A frame with no unwind information ends it only where the entry
            // code began it; anywhere else it is assembly the unwinder cannot
            // step through, a user copy or an exception taken in kernel mode,
            // and the stack goes on below it. Any other failure leaves a
            // prefix too.
            bool const complete =
                step == unwind::StepResult::End ||
                (step == unwind::StepResult::NoFde && is_stack_root(regs, context));
            if (!complete) {
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

auto Recorder::is_stack_root(unwind::Registers const &regs, SampleContext const &context) const
    -> bool {
    for (usize i = 0; i < m_stack_root_count; ++i) {
        auto const &root = m_stack_roots[i];
        if (regs.pc == root.return_address && context.stack_top - regs.sp == root.depth) {
            return true;
        }
    }
    return false;
}

auto Recorder::init(Resources const &resources) -> InitFlags {
    auto const buffers_ready = m_sink.init(Sink::Resources{
        .buffer_lookup = resources.buffer_lookup,
        .buffer_lookup_context = resources.buffer_lookup_context,
        .num_cpus = resources.num_cpus,
    });

    m_eh_section = resources.eh_section;
    m_fde_index.init(resources.fde_entries, resources.fde_entry_count);
    m_image_base = resources.image_base;
    m_stack_roots = resources.stack_roots;
    m_stack_root_count = resources.stack_root_count;

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

auto Recorder::reset() -> void {
    m_sink.reset();
}

auto Recorder::record_sample(SampleContext const &context) -> void {
    auto const cpu = context.cpu;

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.depth = 1;
    record.cpu = static_cast<u16>(cpu);
    record.tid = context.tid;
    record.pid = context.pid;
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

auto Recorder::note_unmatched(u32 const cpu) -> void {
    m_sink.skip(cpu);
}

auto Recorder::record_user_stack(SampleContext const &context, u64 const *const ips,
                                 usize const depth) -> void {
    if (depth == 0) {
        return;
    }

    SampleRecord record = {};
    record.magic = SAMPLE_MAGIC;
    record.flags = RecordFlags::FromUser;
    record.cpu = static_cast<u16>(context.cpu);
    record.tid = context.tid;
    record.pid = context.pid;
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

} // namespace sampler
