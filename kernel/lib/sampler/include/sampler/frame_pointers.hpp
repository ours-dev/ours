/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_FRAME_POINTERS_HPP
#define SAMPLER_FRAME_POINTERS_HPP 1

#include <ours/types.hpp>
#include <unwind/unwinder.hpp>

namespace sampler {
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
} // namespace sampler

#endif // #ifndef SAMPLER_FRAME_POINTERS_HPP
