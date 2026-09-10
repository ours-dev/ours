/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_PERCPU_HPP
#define OURS_ARCH_X86_PERCPU_HPP 1

// Per-CPU addressing: %gs holds `CPU_OFFSETS[cpu]`, i.e. how far the running
// CPU's unit sits from the linker template, so the displacement is the
// variable's own address -- nothing has to be subtracted first. C++ reaches
// the same spots through CpuLocal::read/write/access.
//
// The (%rip) is not optional. The kernel is a static PIE linked at 0 and
// relocated by ElfImage::relocate(), so an absolute displacement would encode
// the link-time address and read the wrong memory once the image moves.
// Linux carries the same suffix for the same reason; see __percpu_rel in its
// arch/x86/include/asm/percpu.h.
#define CPU_LOCAL_ACCESS(VAR) % gs: (VAR)(% rip)

#define X86_PERCPU_SAVED_KERNEL_SP_OFFSET 4
#define X86_PERCPU_SAVED_USER_SP_OFFSET 20

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#include <ours/compiler.hpp>
#include <ours/macro_abi.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/types.hpp>

#include <arch/x86/descriptor.hpp>
#include <ustl/bitset.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours {
struct InterruptStatck {
    constexpr static auto INTR_STACK_SIZE = KB(16);

    FORCE_INLINE
    auto top() -> void * {
        return stack;
    }

    FORCE_INLINE
    auto bottom() -> void * {
        return stack + INTR_STACK_SIZE;
    }

    u8 stack[INTR_STACK_SIZE];
};

enum IntrStackOrdering {
    DOUBLE_FAULT_STACK,
    NMI_STACK,
    DEBUG_BREAK_STACK,
    MACHINE_CHECK_STACK,
    MAX_NUM_STACKS,
};

struct PACKED X86Tss {
    typedef X86Tss Self;

    FORCE_INLINE
    auto init() -> void {
        io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
        io_bitmap[arch::MAX_IO_BITMAP_BYTES] = 0xff;
    }

    // We hope to use two views to access the bitmap.
    u32 reserved0{};
    u64 sp0{};
    u64 sp1{};
    u64 sp2{};
    u64 reserved1{};
    u64 ist1{};
    u64 ist2{};
    u64 ist3{};
    u64 ist4{};
    u64 ist5{};
    u64 ist6{};
    u64 ist7{};
    u64 reserved2{};
    u16 reserved3{};
    u16 io_port_bitmap_base{};
    u8 io_bitmap[arch::MAX_IO_BITMAP_BYTES + 1]{};
};
static_assert(offsetof(X86Tss, sp0) == 4);
static_assert(offsetof(X86Tss, sp2) == 20);

struct alignas(16) X86PerCpu {
    typedef X86PerCpu Self;

    X86Tss tss;
    u32 apic_id;
    u32 padding_;
    CpuNum cpunum;
    InterruptStatck intr_stack[MAX_NUM_STACKS];
};

static_assert(offsetof(X86PerCpu, tss.sp0) == X86_PERCPU_SAVED_KERNEL_SP_OFFSET);
static_assert(offsetof(X86PerCpu, tss.sp2) == X86_PERCPU_SAVED_USER_SP_OFFSET);

/// Using CpuLocal::access<X86PerCpu>() or CpuLocal::access(&X86_PCPU)
/// to access the x86 per CPU area.
///
/// Note the `extern "C"` linkage is necessary because the symbol is being
/// referenced from `ours/arch/x86/main/syscall.S`, which can only name the
/// unmangled spelling.
extern "C" {
CPU_LOCAL
inline X86PerCpu X86_PCPU;
}

} // namespace ours

#endif // !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)
#endif // #ifndef OURS_ARCH_X86_PERCPU_HPP
