/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_VMO_HPP
#define OU_VMO_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ou {
/// Copies |length| bytes of the VMO |vmo|, starting at byte |offset|, into
/// |buffer|. Needs Read on the handle.
///
/// All or nothing: OutOfRange when the range runs past the VMO's end, and
/// nothing is copied then. Pages nobody has written read as zero. InvArgs
/// when |buffer| cannot take |length| bytes; some of it may have been written
/// by then, so after any failure its contents are unspecified.
auto vmo_read(ours::HandleValue vmo, void *buffer, u64 offset, usize length) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_VMO_HPP
