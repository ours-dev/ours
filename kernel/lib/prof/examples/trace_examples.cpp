/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

// Worked examples of the tracepoint interface, in the shape kernel code
// would actually use it. They run: the harness at the bottom of this file
// stands in for the not-yet-written `Tracer` and decodes what was recorded,
// so `main` prints the event stream each example produced.
//
// Build and run (host):
//   clang++ -std=c++2c -I kernel/lib/prof/include -I system/public \
//           -include kernel/include/ours/prelude.hpp \
//           kernel/lib/prof/examples/trace_examples.cpp

#include <prof/trace.hpp>

#include <array>
#include <atomic>
#include <iterator>
#include <string_view>

#include <cstdio>

// ===========================================================================
// Example 1 -- an instant event
//
// A tracepoint is one declaration plus one ordinary function. The function
// is what the rest of the subsystem calls, so the argument types are stated
// once, in a signature, rather than at every call site.
// ===========================================================================
namespace ours::task {

PROF_DECLARE_TRACEPOINT(TP_SCHED_SWITCH, "sched_switch", "sched");

enum class ThreadState : u8 { Running, Blocked, Dead };

inline auto trace_sched_switch(u32 prev_tid, u32 next_tid, ThreadState prev_state) -> void {
    TP_SCHED_SWITCH.emit(prev_tid, next_tid, prev_state);
}

auto example_instant() -> void {
    trace_sched_switch(7, 9, ThreadState::Blocked);
}

} // namespace ours::task

// ===========================================================================
// Example 2 -- a region, and why it is a scope object
//
// `fault` has three ways out. A hand-written `trace_end()` would have to be
// repeated on each of them, and the third one would eventually be missed.
// ===========================================================================
namespace ours::mm {

// The kernel gets this from <ours/types.hpp>; spelled out here so the
// example builds standalone on the host.
using VirtAddr = usize;

PROF_DECLARE_TRACEPOINT(TP_VM_FAULT, "vm_fault", "mm");

auto fault(VirtAddr va, bool present, bool permitted) -> int {
    prof::TraceScope scope{TP_VM_FAULT, va};

    if (!permitted) {
        return -1; // End is still recorded
    }
    if (!present) {
        return -2; // and here
    }
    return 0; // and here
}

auto example_region() -> void {
    auto _ = fault(0x1000, true, false); // denied
    auto _ = fault(0x2000, false, true); // not present
    auto _ = fault(0x3000, true, true);  // handled
}

} // namespace ours::mm

// ===========================================================================
// Example 3 -- scopes are lexical, not per-function
//
// The granularity is whatever block you bracket. Here one function is split
// into two measured phases plus a per-iteration region, which is the kind of
// thing function-entry instrumentation cannot express at all.
// ===========================================================================
namespace ours::mm {

PROF_DECLARE_TRACEPOINT(TP_FIND_VMA, "find_vma", "mm");
PROF_DECLARE_TRACEPOINT(TP_MAP_ONE, "map_one", "mm");

auto map_range(VirtAddr base, usize count) -> void {
    {
        prof::TraceScope scope{TP_FIND_VMA, base};
        // ... locate the area ...
    }
    for (usize i = 0; i < count; ++i) {
        prof::TraceScope scope{TP_MAP_ONE, base + i * 0x1000};
        // ... map one page ...
    }
}

auto example_nested() -> void {
    map_range(0x10000, 3);
}

} // namespace ours::mm

// ===========================================================================
// Example 4 -- an argument that is expensive to produce
//
// Arguments are evaluated whether or not the site is on: this is a function
// call, not a macro. When producing one actually costs something, ask first.
// ===========================================================================
namespace ours::mm {

PROF_DECLARE_TRACEPOINT(TP_HEAP_STATS, "heap_stats", "mm");

auto count_free_pages() -> u64 {
    std::printf("      [count_free_pages() ran]\n");
    return 4096;
}

auto example_guarded() -> void {
    // Wrong: count_free_pages() runs even when nobody is listening.
    //     TP_HEAP_STATS.emit(count_free_pages());

    // Right: the guard is explicit and reads as what it is.
    if (TP_HEAP_STATS.enabled()) {
        TP_HEAP_STATS.emit(count_free_pages());
    }
}

} // namespace ours::mm

// ===========================================================================
// Example 5 -- pointers and enums
//
// Both go through `to_slot`: a pointer as its address, an enum as its
// underlying value. The tracepoint's own signature is what says how to read
// the slot back, so no type tag has to travel with the record.
// ===========================================================================
namespace ours::sync {

PROF_DECLARE_TRACEPOINT(TP_LOCK_CONTENDED, "lock_contended", "sync");

enum class LockKind : u8 { Spin, Mutex, RwRead, RwWrite };

inline auto trace_lock_contended(void const *lock, LockKind kind, u32 owner_tid) -> void {
    TP_LOCK_CONTENDED.emit(lock, kind, owner_tid);
}

auto example_mixed_args() -> void {
    static int a_lock = 0;
    trace_lock_contended(&a_lock, LockKind::Mutex, 42);
}

} // namespace ours::sync

// ===========================================================================
// Example 6 -- a site that is off records nothing
// ===========================================================================
namespace ours::irq {

PROF_DECLARE_TRACEPOINT(TP_IRQ_ENTER, "irq_enter", "irq");

auto example_disabled() -> void {
    TP_IRQ_ENTER.emit(u32{32}); // left off by main(); produces no record
}

} // namespace ours::irq

// ===========================================================================
// Harness. `Tracer` does all of this for real; it is inlined here so the
// examples above are runnable rather than illustrative.
// ===========================================================================
namespace {

prof::Tracepoint const *g_tracepoints[] = {
    &ours::task::TP_SCHED_SWITCH, &ours::mm::TP_VM_FAULT,   &ours::mm::TP_FIND_VMA,
    &ours::mm::TP_MAP_ONE,        &ours::mm::TP_HEAP_STATS, &ours::sync::TP_LOCK_CONTENDED,
    &ours::irq::TP_IRQ_ENTER,
};

std::array<prof::TraceRecord, 64> g_slots;
prof::RecordBuffer<prof::TraceRecord> g_buffer;
u64 g_fake_clock = 1000;

auto find_name(u16 id) -> char const * {
    return id < std::size(g_tracepoints) ? g_tracepoints[id]->name() : "?";
}

auto kind_name(prof::EventKind kind) -> char const * {
    switch (kind) {
        case prof::EventKind::Instant:
            return "instant";
        case prof::EventKind::Begin:
            return "begin  ";
        case prof::EventKind::End:
            return "end    ";
        default:
            return "?";
    }
}

auto drain() -> void {
    g_buffer.for_each_retained([](auto const &r) {
        std::printf("      t=%-5llu %s %-14s", static_cast<unsigned long long>(r.m_metric),
                    kind_name(r.m_kind), find_name(r.m_event_id));
        for (u8 a = 0; a < r.m_arg_count; ++a) {
            std::printf(" 0x%llx", static_cast<unsigned long long>(r.m_args[a]));
        }
        std::printf("\n");
    });
    g_buffer.reset();
}

} // namespace

// What the kernel's Tracer will read from the kernel itself -- the CPU, the
// thread, the time -- stubbed for the host. Not an arch seam: none of the
// three is a fact about the architecture.
namespace prof::host {
auto current_cpu() -> u32 {
    return 0;
}
auto current_tid() -> u32 {
    return 1;
}
auto timestamp() -> u64 {
    return g_fake_clock++;
}
} // namespace prof::host

// The write path `Tracer` will own. This is the whole of it: build a fixed
// record on the stack and hand it to the per-CPU sink. No allocation, no
// lock, no loop.
namespace prof {
template <TraceArg... Args>
auto Tracepoint::write(EventKind kind, Args... args) const noexcept -> void {
    if constexpr (!TRACE_ENABLED) {
        return;
    } else {
        if (!m_enabled.load(std::memory_order_relaxed)) {
            return;
        }
        TraceRecord record{};
        record.m_kind = kind;
        record.m_arg_count = sizeof...(Args);
        record.m_event_id = m_id;
        record.m_cpu = static_cast<u16>(host::current_cpu());
        record.m_tid = host::current_tid();
        record.m_metric = host::timestamp();

        usize slot = 0;
        ((record.m_args[slot++] = to_slot(args)), ...);

        auto _ = g_buffer.try_produce(record);
    }
}
} // namespace prof

auto main() -> int {
    // What Tracer::init() does: give the buffer its slots, walk the section,
    // hand out ids.
    g_buffer.attach(g_slots.data(), g_slots.size());
    for (u16 id = 0; id < std::size(g_tracepoints); ++id) {
        g_tracepoints[id]->assign_id(id);
    }
    // What `trace.on <category>` does. Note irq is deliberately left off.
    for (auto const *tp: g_tracepoints) {
        tp->set_enabled(std::string_view{tp->category()} != "irq");
    }

    std::printf("1) instant event\n");
    ours::task::example_instant();
    drain();

    std::printf("2) region with three exits\n");
    ours::mm::example_region();
    drain();

    std::printf("3) two phases + one region per iteration\n");
    ours::mm::example_nested();
    drain();

    std::printf("4) expensive argument, guarded (site is on, so it runs)\n");
    ours::mm::example_guarded();
    drain();
    std::printf("   ... same call with the site off:\n");
    ours::mm::TP_HEAP_STATS.set_enabled(false);
    ours::mm::example_guarded();
    drain();

    std::printf("5) pointer and enum arguments\n");
    ours::sync::example_mixed_args();
    drain();

    std::printf("6) a site that is off\n");
    ours::irq::example_disabled();
    drain();
    std::printf("   (nothing above -- correct)\n");
    return 0;
}
