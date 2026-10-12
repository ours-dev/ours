/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROFILE_DESCRIBE_HPP
#define PROFILE_DESCRIBE_HPP 1

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

#include <array>

#include <eftl/elf/details/segment.hpp>

namespace profile {
/// The longest build ID kept: a SHA-256 is 32 bytes, and the build here asks
/// for SHA-1, 20 (//build/config:build_id).
inline constexpr usize MAX_BUILD_ID = 32;

/// The most loaded segments kept for one module, and modules for one
/// process: a program here maps its own ELF and the vDSO, and nothing else.
inline constexpr usize MAX_SEGMENTS = 8;
inline constexpr usize MAX_MODULES = 4;

/// One loaded segment of a module, in whole pages: where it lies in the
/// process's memory, how long it is, its rights, and where it lies in the
/// ELF's own addresses, which is what a symbolizer looks an address up by.
struct LoadedSegment {
    u64 start;
    u64 size;
    eftl::elf::SegPerms permissions;
    u64 relative;
};

/// One ELF a process runs code from, as symbolizer markup describes it.
struct Module {
    /// The koid of the VMO its code is mapped from, which tells one module
    /// from another within a process.
    ours::KoId vmo;
    /// The VMO's name, cut to what markup may hold; may be empty.
    std::array<char, ours::MAX_NAME_LEN> name;
    usize name_size;
    std::array<u8, MAX_BUILD_ID> build_id;
    usize build_id_size;
    std::array<LoadedSegment, MAX_SEGMENTS> segments;
    usize num_segments;

    /// Whether |address| lies in one of its executable segments.
    [[nodiscard]]
    auto runs(u64 address) const -> bool;
};

/// What a process's samples need to be named: its koid, which its user
/// records carry as pid, and the modules its code comes from.
struct Process {
    ours::KoId koid;
    std::array<Module, MAX_MODULES> modules;
    usize num_modules;

    /// Whether |address| lies in an executable segment of any of its modules.
    [[nodiscard]]
    auto runs(u64 address) const -> bool;
};

/// Describes the process |handle| names, whose koid is |koid|, from outside
/// it: its mappings (object_get_info ProcessMaps, which needs Inspect), and
/// for each VMO it maps executable, the ELF headers and build ID note read
/// from the mapping of that VMO at offset 0 (process_read_memory, which
/// needs Read and Write). A VMO with no mapping at offset 0, or whose bytes
/// there are not an ELF with a build ID, is left out; so are modules past
/// MAX_MODULES. Errors are the system calls' on the maps; a process with no
/// module that could be read is Ok, and describes nothing.
[[nodiscard]]
auto describe(ours::Uandle handle, ours::KoId koid, Process *out) -> ours::Status;
} // namespace profile

#endif // #ifndef PROFILE_DESCRIBE_HPP
