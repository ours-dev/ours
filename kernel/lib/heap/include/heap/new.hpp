/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef HEAP_NEW_HPP
#define HEAP_NEW_HPP 1

#include <heap/kmalloc.hpp>

#include <new>

/// The kernel's `new`, and the one place the choice between the two
/// allocators is made.
///
/// `new (gaf) T` serves any size: at or below heap::KMALLOC_MAX_SIZE it comes
/// from a kmalloc size class, above it from vmalloc's window. Callers do not
/// choose and should not have to -- where the boundary sits is a property of
/// the size-class ladder, not of the object being allocated, and it moves if
/// the ladder grows.
///
/// `noexcept`, and that is load-bearing rather than decorative. Without it the
/// language lets the compiler assume the allocation function never returns
/// null, so `new (gaf) T(...)` runs T's constructor on address zero and the
/// caller's `if (!p)` is never reached -- which is how a failed ktl::Vec
/// growth became a page fault instead of an OutOfMemory (see
/// docs/repairs/260903/ktl.md). With it, a new-expression whose allocation
/// returns null evaluates to null without constructing anything.
///
/// Alignment: kmalloc answers alignof(usize), vmalloc answers a whole page.
/// There is no `std::align_val_t` overload here, so an over-aligned type
/// allocated through these forms is silently under-aligned on the kmalloc
/// side. Nothing in the tree does that today; see docs/repairs/260908/heap.md.
///
/// Above the threshold these inherit vmalloc's calling contract: they may
/// block, so they must not be used from an interrupt handler or with a spin
/// lock held. Below it they behave exactly as they always have.
[[nodiscard]]
auto operator new(usize size, ours::mm::Gaf gaf) noexcept -> void *;

[[nodiscard]]
auto operator new[](usize size, ours::mm::Gaf gaf) noexcept -> void *;

/// The node-affine forms. Only kmalloc honours the node hint; a request large
/// enough to reach vmalloc is served from wherever the buddy allocator has
/// room, because a virtually contiguous range is stitched from scattered
/// frames and pinning all of them to one node would be a much stronger
/// promise than the caller asked for.
[[nodiscard]]
auto operator new(usize size, ours::mm::Gaf gaf, ours::NnId nid) noexcept -> void *;

[[nodiscard]]
auto operator new[](usize size, ours::mm::Gaf gaf, ours::NnId nid) noexcept -> void *;

/// Deallocation routes on the address, not the size.
///
/// The two allocators return addresses from disjoint regions -- kmalloc's out
/// of the physmap, vmalloc's out of its own window -- so a range test answers
/// the question in two instructions, and it is the only thing that can:
/// there is no header on either allocation, and the size, where the compiler
/// supplies one at all, describes the object rather than the allocation.
///
/// Every overload the compiler can emit is defined here, including the sized
/// and array forms. They all forward to the same routing function. Leaving
/// any of them out does not produce a link error -- //kernel/lib/libc carries
/// weak no-op stubs for the ones libc++ references -- it produces a silent
/// leak, which is why they are spelled out rather than left to chance.
///
/// The deallocation functions carry noexcept because the standard gives them
/// that specification; libc++'s <new> insists on it when a kernel header lands
/// in a hosted build (the unit tests).
auto operator delete(void *ptr) noexcept -> void;

auto operator delete[](void *ptr) noexcept -> void;

auto operator delete(void *ptr, usize size) noexcept -> void;

auto operator delete[](void *ptr, usize size) noexcept -> void;

#endif // #ifndef HEAP_NEW_HPP
