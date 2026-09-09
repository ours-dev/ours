/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_VDSO_HPP
#define OURS_VDSO_HPP 1

// The contract between the vDSO, the kernel that maps it, and the program that
// calls through it.
//
// The vDSO is the only image in a user address space that contains a `syscall`
// instruction, and the kernel refuses a trap whose return address lies outside
// it (see kernel/lib/syscall/table.cpp). A user program therefore never traps
// directly; it calls a vDSO entry, which traps on its behalf.
//
// Written so an assembler can read it: the vDSO's stubs and its entry table are
// written in assembly, since only there can a table hold link-time-computed
// offsets to the functions beside it.

/// Identifies the entry table, so a mis-loaded image is refused rather than
/// called into.
///
/// No digit separator: this header is included by the vDSO's assembly, and the
/// assembler does not accept C++'s `'`.
#define OURS_VDSO_MAGIC 0x4F564453 // "OVDS"

/// Bumped whenever the table's shape changes -- a field added, reordered, or
/// given a new meaning. The kernel checks it, so an old vDSO packaged beside a
/// new kernel fails at load rather than at the first call.
#define OURS_VDSO_VERSION 1

/// Byte offsets into the entry table. The assembler needs them as constants and
/// the C++ side gets the same layout from the struct below; the static_asserts
/// there are what keep the two in step.
#define OURS_VDSO_TABLE_OFF_MAGIC 0
#define OURS_VDSO_TABLE_OFF_VERSION 4
#define OURS_VDSO_TABLE_OFF_NR_ENTRIES 6
#define OURS_VDSO_TABLE_OFF_DEBUG_WRITE 8
#define OURS_VDSO_TABLE_SIZE 12

/// How many entries version 1 publishes.
#define OURS_VDSO_NR_ENTRIES 1

#ifdef __cplusplus

#include <ours/types.hpp>

namespace ours {
/// The table the vDSO publishes at its ELF entry point.
///
/// Reached through `e_entry` rather than through a symbol lookup, which is what
/// lets the first user program find it with no dynamic linker and no symbol
/// table walk: the kernel already reads e_entry to load an image, and for an
/// object that is never jumped to, "the entry" is the only thing it could
/// usefully mean. The kernel passes the table's runtime address to the program
/// in its first start argument.
///
/// Every entry is a byte offset **from the start of this table**, not a
/// pointer. Nothing relocates the vDSO -- the kernel maps it where it likes and
/// applies no relocations -- so an absolute address stored here would be
/// whatever the linker guessed and would point nowhere. An offset is correct at
/// any load address, which is the same reason userboot's own image has no
/// relocations.
struct VdsoEntryTable {
    u32 magic;
    u16 version;
    u16 nr_entries;

    /// ours_debug_write(char const *buffer, usize length) -> Status
    u32 debug_write;
};

static_assert(offsetof(VdsoEntryTable, magic) == OURS_VDSO_TABLE_OFF_MAGIC);
static_assert(offsetof(VdsoEntryTable, version) == OURS_VDSO_TABLE_OFF_VERSION);
static_assert(offsetof(VdsoEntryTable, nr_entries) == OURS_VDSO_TABLE_OFF_NR_ENTRIES);
static_assert(offsetof(VdsoEntryTable, debug_write) == OURS_VDSO_TABLE_OFF_DEBUG_WRITE);
static_assert(sizeof(VdsoEntryTable) == OURS_VDSO_TABLE_SIZE);

} // namespace ours

#endif // __cplusplus

#endif // OURS_VDSO_HPP
