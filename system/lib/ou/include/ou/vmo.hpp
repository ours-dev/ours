/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OU_VMO_HPP
#define OU_VMO_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

namespace ou {
/// Makes a VMO of |size| bytes, rounded up to a page, and puts a handle to it
/// in |out|. Committed lazily; unwritten pages read as zero. |options| must be
/// None (Resizable is NotSupported). The handle never has Execute.
auto vmo_create(u64 size, ours::VmoOptions options, ours::HandleValue *out) -> ours::Status;

/// Copies |length| bytes of the VMO |vmo|, starting at byte |offset|, into
/// |buffer|. Needs Read on the handle.
///
/// All or nothing: OutOfRange when the range runs past the VMO's end, and
/// nothing is copied then. Pages nobody has written read as zero. InvArgs
/// when |buffer| cannot take |length| bytes; some of it may have been written
/// by then, so after any failure its contents are unspecified.
auto vmo_read(ours::HandleValue vmo, void *buffer, u64 offset, usize length) -> ours::Status;

/// A child of the VMO |vmo| -- options must be Slice today -- into |out|: the
/// window [offset, offset + size) of it, over the same pages, with |vmo|'s
/// rights. |vmo| must grant Duplicate and Read.
auto vmo_create_child(ours::HandleValue vmo, ours::VmoChildOptions options, u64 offset, u64 size,
                      ours::HandleValue *out) -> ours::Status;

/// Replaces |vmo| with a handle to the same VMO that may be executed, into
/// |out|: |vmo|'s rights less Write, plus Execute. |vmex| must be the VMEX
/// System resource. |vmo| is consumed whatever the answer; duplicate it first
/// (ou::handle_duplicate) to keep a copy.
auto vmo_replace_as_executable(ours::HandleValue vmo, ours::HandleValue vmex,
                               ours::HandleValue *out) -> ours::Status;

/// Copies |length| bytes from |buffer| into the VMO |vmo|, starting at byte
/// |offset|. Needs Write on the handle.
///
/// OutOfRange when the range runs past the VMO's end, and nothing is written
/// then. InvArgs when |buffer| cannot give |length| bytes; part of the range
/// may have been written by then, so after any failure its contents are
/// unspecified.
auto vmo_write(ours::HandleValue vmo, void const *buffer, u64 offset, usize length) -> ours::Status;
} // namespace ou

#endif // #ifndef OU_VMO_HPP
