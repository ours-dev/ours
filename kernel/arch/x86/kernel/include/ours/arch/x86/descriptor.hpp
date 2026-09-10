/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_DESCRIPTOR_HPP
#define OURS_ARCH_X86_DESCRIPTOR_HPP 1

#define X86_GDT_KERNEL_CODE32 (1 << 3)
#define X86_GDT_KERNEL_CODE64 (2 << 3)
#define X86_GDT_KERNEL_DATA (3 << 3)

/// The user selectors carry RPL 3, the kernel ones RPL 0.
///
/// A selector is a GDT index *and* a requested privilege level, and the
/// low two bits are that level. These three had the index only, so
/// X86_GDT_USER_CODE64 was 0x30 -- index 6, RPL 0. An `iret` down to ring 3
/// checks CS.RPL against the descriptor it names and refuses to lower the
/// privilege level when the selector asks for ring 0, so the first attempt to
/// enter user mode raised #GP inside the iret and triple-faulted from there.
/// Nothing had entered ring 3 before, so nothing had needed the two bits.
///
/// Zircon writes the same thing the same way: USER_CODE_64_SELECTOR is
/// (0x30 | 3) and USER_DATA_SELECTOR is (0x28 | 3), over the same GDT layout.
/// The 32-bit user selector needs it too: it is what goes into the STAR MSR
/// for sysret, which likewise returns to ring 3.
#define X86_GDT_USER_CODE32 ((4 << 3) | 3)
#define X86_GDT_USER_DATA ((5 << 3) | 3)
#define X86_GDT_USER_CODE64 ((6 << 3) | 3)
#define X86_GDT_MAX_SELECTORS (6 + 1) // Has also the first null descriptor
#define X86_SEGMENT_SELECTOR_SIZE 8

#define TSS_SELECTOR(i) ((u16)(0x38 + 16 * (i)))

/// Selector privilege level
#define SELECTOR_PL(s) ((s) & 0x3)

/// Descriptor types
#define SEG_TYPE_TSS 0x9
#define SEG_TYPE_TSS_BUSY 0xb
#define SEG_TYPE_TASK_GATE 0x5
#define SEG_TYPE_INT_GATE 0xe /* 32 bit */
#define SEG_TYPE_DATA_RW 0x2
#define SEG_TYPE_CODE_RW 0xa

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#include <ours/compiler.hpp>
#include <ours/mm/cfg.hpp>

#include <array>

#include <arch/x86/descriptor.hpp>

namespace ours {
auto x86_get_gdt() -> void *;

auto x86_load_gdt() -> void;

auto x86_dump_gdt() -> void;

auto x86_set_tss_sp(VirtAddr sp) -> void;

} // namespace ours
#endif // !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#endif // #ifndef OURS_ARCH_X86_DESCRIPTOR_HPP
