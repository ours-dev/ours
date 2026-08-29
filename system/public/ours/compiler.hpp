/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_COMPILER_HPP
#define OURS_COMPILER_HPP 1

#define USED [[gnu::used]]
#define NAKED [[gnu::naked]]
#define PACKED [[gnu::packed]]

#define NO_INLINE [[gnu::noinline]]
#define NO_RETURN [[noreturn]]
// An attribute and nothing else. Note that it only tells the optimiser what to
// do -- it confers no inline linkage, so a function *defined* in a header still
// needs `inline` written out, or every translation unit including it emits its
// own copy and the linker reports a duplicate symbol.
#if defined(_MSC_VER)
#define FORCE_INLINE [[msvc::force_inline]]
#else
#define FORCE_INLINE [[gnu::always_inline]]
#endif

// Spelling used where a declaration specifier cannot go -- on a lambda, for
// instance: `[] FORCE_INLINE_ATTR(int x) {...}`.
#define FORCE_INLINE_ATTR FORCE_INLINE

#define WEAK_LINK [[gnu::weak]]
#define LINK_SECTION(name) [[gnu::section(name)]]
#define LINK_NAME(name) asm(name)

#define NO_MANGLE extern "C"

// Part of physload's module ABI.
//
// The build force-includes ours/hidden.hpp, so every symbol is hidden by
// default and a physload-loaded module cannot bind to it.  This attribute on
// a declaration is what puts the symbol in physload's export table and lets
// a module resolve it at load time; see ours/phys/exports.hpp.
//
// Visibility only: it goes on declarations, where `used`/`retain` do not
// apply.  Keeping the definitions alive is the link's job -- physload's
// first pass runs without --gc-sections so that a function only modules
// call still reaches the export table.
#define PHYS_EXPORT [[gnu::visibility("default")]]

#if defined(_WIN32)
#define KERNEL_API
#else
#define KERNEL_API __attribute__((visibility("hidden")))
#endif

// The given C++ class or struct need not have a unique address when part of
// a larger struct or class, but can be safely collapsed into a zero-byte
// object by the compiler.
#if defined(__ASSEMBLER__) || defined(__ASSEMBLY__)
#define NO_UNIQUE_ADDRESS
#elif __has_cpp_attribute(no_unique_address)
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#elif __has_cpp_attribute(msvc::no_unique_address)
// msvc targets, as used by efi, have a different attribute name, even when
// targeting C++20.
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS
#endif

#ifdef __clang__
#define THREAD_ANNOTATE(x) __attribute__((x))
#else
#define THREAD_ANNOTATE(...)
#endif

#define TA_CAPABILITY(x) THREAD_ANNOTATE(__capability__(x))
#define TA_GUARDED(x) THREAD_ANNOTATE(__guarded_by__(x))
#define TA_ACQUIRE(...) THREAD_ANNOTATE(__acquire_capability__(__VA_ARGS__))
#define TA_ACQUIRE_SHARED(...) THREAD_ANNOTATE(__acquire_shared_capability__(__VA_ARGS__))
#define TA_TRY_ACQUIRE(...) THREAD_ANNOTATE(__try_acquire_capability__(__VA_ARGS__))
#define TA_TRY_ACQUIRE_SHARED(...) THREAD_ANNOTATE(__try_acquire_shared_capability__(__VA_ARGS__))
// Neither ACQUIRED_BEFORE nor ACQUIRED_AFTER are implemented in clang. Users of these macros must
// not rely upon them to catch lock ordering bugs, and must treat them as documentation only.
// See:
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html#acquired-before-and-acquired-after-are-currently-unimplemented
#define TA_ACQUIRED_BEFORE(...) THREAD_ANNOTATE(__acquired_before__(__VA_ARGS__))
#define TA_ACQUIRED_AFTER(...) THREAD_ANNOTATE(__acquired_after__(__VA_ARGS__))
#define TA_RELEASE(...) THREAD_ANNOTATE(__release_capability__(__VA_ARGS__))
#define TA_RELEASE_SHARED(...) THREAD_ANNOTATE(__release_shared_capability__(__VA_ARGS__))
#define TA_REQUIRES(...) THREAD_ANNOTATE(__requires_capability__(__VA_ARGS__))
#define TA_REQUIRES_SHARED(...) THREAD_ANNOTATE(__requires_shared_capability__(__VA_ARGS__))
#define TA_EXCLUDES(...) THREAD_ANNOTATE(__locks_excluded__(__VA_ARGS__))
#define TA_ASSERT(...) THREAD_ANNOTATE(__assert_capability__(__VA_ARGS__))
#define TA_ASSERT_SHARED(...) THREAD_ANNOTATE(__assert_shared_capability__(__VA_ARGS__))
#define TA_RETURN_CAPABILITY(x) THREAD_ANNOTATE(__lock_returned__(x))
#define TA_SCOPED_CAPABILITY THREAD_ANNOTATE(__scoped_lockable__)
#define TA_NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATE(__no_thread_safety_analysis__)

// Compatibility spellings used by the pre-refactor kernel headers.
#define TA_CAP(x) TA_CAPABILITY(x)
#define TA_GUARD(x) TA_GUARDED(x)
#define TA_REQUIRE(...) TA_REQUIRES(__VA_ARGS__)

#endif // #ifndef OURS_COMPILER_HPP
