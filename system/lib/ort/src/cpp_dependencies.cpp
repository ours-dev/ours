/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// What a user program's C++ needs from the runtime that no library here
/// provides, as //kernel/lib/libc's cpp_dependencies.cpp does for the kernel.
#include <new>

#include <cstddef>

namespace {
/// A user program has no heap yet, so no pointer can be one operator new
/// returned. Null is the one value delete must take, as a no-op
/// ([new.delete.single]); any other value is a bug, and the program stops
/// at it instead of carrying on with its memory in an unknown state.
auto refuse_any_but_null(void *const pointer) -> void {
    if (pointer != nullptr) {
        __builtin_trap();
    }
}
} // namespace

/// operator delete, and no operator new: an accidental `new` stays a link
/// error, which names the line that wrote it. delete has to exist all the
/// same, because every class with a virtual destructor gets a "deleting
/// destructor" (the Itanium C++ ABI's D0) in its vtable, which calls it,
/// whether or not an object of the class is ever on a heap.
/// eftl::elf::BasicImage, which the launcher pulls in through elfload, is one.
/// Weak, as the kernel's are, so that an allocator linked in later replaces
/// them. Each sized form has its unsized partner, as [new.delete.single]
/// requires of a program that defines the sized one.
[[gnu::weak]]
auto operator delete(void *pointer) noexcept -> void {
    refuse_any_but_null(pointer);
}

[[gnu::weak]]
auto operator delete(void *pointer, std::size_t) noexcept -> void {
    refuse_any_but_null(pointer);
}

[[gnu::weak]]
auto operator delete(void *pointer, std::align_val_t) noexcept -> void {
    refuse_any_but_null(pointer);
}

[[gnu::weak]]
auto operator delete(void *pointer, std::size_t, std::align_val_t) noexcept -> void {
    refuse_any_but_null(pointer);
}

/// What a pure virtual function's slot in a vtable points to, so that a call
/// to one -- through an object still being built or already being destroyed,
/// say -- lands somewhere defined. eftl::Binary, the base of BasicImage, is
/// abstract, and its vtable names this wherever the optimizer keeps it, as
/// -O0 does. Such a call is undefined behaviour, and the Itanium C++ ABI
/// (3.2.6) expects this to end the program; the kernel's returns instead,
/// into whatever comes next, which is not a pattern to copy.
extern "C" [[gnu::weak, noreturn]]
// NOLINTNEXTLINE(readability-identifier-naming) — the ABI names it
auto __cxa_pure_virtual() -> void {
    __builtin_trap();
}
