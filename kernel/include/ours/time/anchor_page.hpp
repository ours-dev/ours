/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TIME_ANCHOR_PAGE_HPP
#define OURS_TIME_ANCHOR_PAGE_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::time {
/// The anchor a monotonic read scales, without the page's sequence word.
/// |denominator| is 1 and the rest zero when the steady clock has no source.
struct AnchorReading {
    u64 ticks = 0;
    i64 nanoseconds = 0;
    u32 numerator = 0;
    u32 denominator = 1;
};

/// Copies |bytes| into the published page at |offset|. An 8-byte copy is one
/// store: the sequence word is read on another CPU and a torn one never
/// finishes the odd-then-even protocol.
using AnchorStore = auto (*)(void *context, u64 offset, void const *bytes, usize size) -> Status;

/// Remembers where the page is and writes the steady clock's anchor into it.
/// Later source switches write it again. Before this, a publish does nothing.
auto bind_anchor_page(AnchorStore store, void *context, u64 offset) -> void;

/// The user virtual address a monotonic read loads the counter from. 0 means
/// none: a publish then writes accessible 0 and counter 0. A later publish
/// writes this address with accessible 1.
auto bind_counter_va(VirtAddr va) -> void;

/// Publishes |reading| into the bound page. No-op until bind_anchor_page().
auto publish_anchor(AnchorReading const &reading) -> void;
} // namespace ours::time

#endif // #ifndef OURS_TIME_ANCHOR_PAGE_HPP
