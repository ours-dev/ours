/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_BUILD_ID_NOTE_HPP
#define OURS_PHYS_BUILD_ID_NOTE_HPP 1

#include <ours/types.hpp>

#include <span>

namespace ours::phys {

/// Hex-encode the sole GNU/NT_GNU_BUILD_ID note in `note_section` into
/// `out`.  Returns the hex length, or 0 for any malformed, absent, or
/// multiple-note input -- 0 is never a valid length (a real build ID has a
/// nonzero digest), so callers can treat it uniformly as "unavailable"
/// without a separate error channel.
///
/// Split out of build-id.cpp so it can be exercised on the host against
/// real note bytes (build-id-note-smoke.cpp): own_build_id() itself needs
/// symbols only a linked phys image defines, but this parsing has nothing
/// phys-specific about it.
[[nodiscard]]
auto hex_encode_build_id(std::span<u8 const> note_section, std::span<char> out) -> usize;

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_BUILD_ID_NOTE_HPP
