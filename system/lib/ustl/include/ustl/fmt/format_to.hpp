// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <format>

#include <cstddef>

namespace ustl::fmt {

using std::format_to;
using std::format_to_n;
using std::vformat_to;

/// Writes at most |n| characters of the formatted text at |out| — a pointer
/// the caller bounds — and answers where the write ended and how much the
/// format demanded.
///
/// This is the one bounded shape the standard machinery runs without an
/// allocator: a raw pointer takes libc++'s direct-write path, capped by the
/// bound. The unbounded vformat_to over a class-shaped output iterator is
/// a trap beside it — that shape buffers through an intermediate which
/// silently grows past its first 256 bytes with operator new, and a kernel
/// whose operator new is a deliberate panic loses the machine to any log
/// line or panic message long enough to cross it. Callers that bound their
/// sink write through this; |result.size| is where they read their
/// truncation from, for it counts the text the format would have written
/// had |n| been larger.
[[nodiscard]]
inline auto vformat_to_n(char *const out, std::ptrdiff_t const n, std::string_view const fmt,
                         std::format_args const args) -> std::format_to_n_result<char *> {
    return std::__vformat_to_n<std::format_context>(out, n, fmt, args);
}

} // namespace ustl::fmt
