/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/time/anchor_page.hpp>
#include <ours/time/clock.hpp>
#include <ours/time_values.hpp>

namespace ours::time {
namespace {
    AnchorStore STORE = nullptr;
    void *CONTEXT = nullptr;
    u64 OFFSET = 0;
    /// The generation the page's sequence word is at. Even means the other words
    /// agree. Kept here so a publish does not have to read the page back: the
    /// page starts at zero, and every publish advances this by two.
    u64 SEQ = 0;
    /// User VA of the counter, or 0 when ring 3 has no mapping of it.
    VirtAddr COUNTER_VA{};
    /// ~0 until lay_out_vdso binds the single-word records.
    u64 TICKS_OFFSET = ~u64{0};
    u64 BOOT_OFFSET_AT = ~u64{0};

    struct Payload {
        u64 ticks;
        i64 nanoseconds;
        u32 numerator;
        u32 denominator;
        u8 accessible;
        u8 pad[7];
        u64 counter;
    };

    static_assert(offsetof(ours::TimeValues, ticks) == 16);
    static_assert(offsetof(ours::TimeValues, counter) == 48);
    static_assert(sizeof(Payload) == 40);
} // namespace

auto bind_anchor_page(AnchorStore const store, void *const context, u64 const offset) -> void {
    STORE = store;
    CONTEXT = context;
    OFFSET = offset;
    SEQ = 0;
    publish_anchor(SteadyClock::reading());
}

auto bind_counter_va(VirtAddr const va) -> void {
    COUNTER_VA = va;
    publish_anchor(SteadyClock::reading());
}

auto bind_ticks_per_second(u64 const offset) -> void {
    TICKS_OFFSET = offset;
}

auto bind_boot_offset(u64 const offset) -> void {
    BOOT_OFFSET_AT = offset;
}

auto publish_anchor(AnchorReading const &reading) -> void {
    if (STORE == nullptr) {
        return;
    }

    // Odd first, so a reader who catches the payload mid-copy retries.
    // |accessible| is 1 only when a user VA of the counter has been bound:
    // a sample of a different counter scaled by this anchor is not a time.
    SEQ += 1;
    auto const odd = SEQ;
    if (STORE(CONTEXT, OFFSET + offsetof(ours::TimeValues, seq), &odd, sizeof(odd)) != Status::Ok) {
        // The page was not touched, so the kernel's count goes back too. A
        // page left odd makes every read spin.
        SEQ -= 1;
        return;
    }
    u8 const accessible = static_cast<u8>(COUNTER_VA != VirtAddr{0});
    Payload const payload{
        .ticks = reading.ticks,
        .nanoseconds = reading.nanoseconds,
        .numerator = reading.numerator,
        .denominator = reading.denominator,
        .accessible = accessible,
        .pad = {},
        .counter = std::to_underlying(COUNTER_VA),
    };
    auto const stored =
        STORE(CONTEXT, OFFSET + offsetof(ours::TimeValues, ticks), &payload, sizeof(payload));
    SEQ += 1;
    auto const even = SEQ;
    auto _ = STORE(CONTEXT, OFFSET + offsetof(ours::TimeValues, seq), &even, sizeof(even));
    if (stored != Status::Ok) {
        return;
    }
    if (TICKS_OFFSET != ~u64{0}) {
        auto const hz = reading.frequency_hz;
        (void)STORE(CONTEXT, TICKS_OFFSET, &hz, sizeof(hz));
    }
    if (BOOT_OFFSET_AT != ~u64{0}) {
        auto const boot = SystemClock::boot_offset();
        (void)STORE(CONTEXT, BOOT_OFFSET_AT, &boot, sizeof(boot));
    }
}
} // namespace ours::time
