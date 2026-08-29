/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_X86_PERCPU_HPP
#define OURS_ARCH_X86_PERCPU_HPP 1

#ifdef OURS_CONFIG_KASLR
#define CPU_LOCAL_ACCESS(VAR) % gs: (VAR)(% rip)
#else
#define CPU_LOCAL_ACCESS(VAR) % gs: VAR
#endif

#define X86_PERCPU_SAVED_KERNEL_SP_OFFSET 4
#define X86_PERCPU_SAVED_USER_SP_OFFSET 20

#if !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)

#include <arch/x86/descriptor.hpp>
#include <ours/compiler.hpp>
#include <ours/macro_abi.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/types.hpp>
#include <ustl/bitset.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours {
struct InterruptStatck {
    constexpr static auto const kIntrStackSize = KB(16);

    FORCE_INLINE
    auto top() -> void * {
        return stack;
    }

    FORCE_INLINE
    auto bottom() -> void * {
        return stack + kIntrStackSize;
    }

    u8 stack[kIntrStackSize];
};

enum IntrStackOrdering {
    kDoubleFaultStack,
    kNmiStack,
    kDebugBreakStack,
    kMachineCheckStack,
    kMaxNumStacks,
};

struct PACKED X86Tss {
    typedef X86Tss Self;

    FORCE_INLINE
    auto init() -> void {
        io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
        io_bitmap[arch::kMaxIoBitmapBytes] = 0xff;
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
    u8 io_bitmap[arch::kMaxIoBitmapBytes + 1]{};
};
static_assert(offsetof(X86Tss, sp0) == 4);
static_assert(offsetof(X86Tss, sp2) == 20);

struct alignas(16) X86PerCpu {
    typedef X86PerCpu Self;

    X86Tss tss;
    u32 apic_id;
    u32 padding_;
    CpuNum cpunum;
    InterruptStatck intr_stack[kMaxNumStacks];
};

static_assert(offsetof(X86PerCpu, tss.sp0) == X86_PERCPU_SAVED_KERNEL_SP_OFFSET);
static_assert(offsetof(X86PerCpu, tss.sp2) == X86_PERCPU_SAVED_USER_SP_OFFSET);

/// Using CpuLocal::access<X86PerCpu>() or CpuLocal::access(&g_x86_pcpu)
/// to access the x86 per CPU area.
///
/// Note the marker `NO_MANGLE` is necessary because the symbol is beeing used
/// in `ours/arch/x86/main/syscall.S`
NO_MANGLE {
    CPU_LOCAL
    inline X86PerCpu g_x86_pcpu;
}

} // namespace ours

#endif // !defined(__ASSEMBLY__) && !defined(__ASSEMBLER__)
#endif // #ifndef OURS_ARCH_X86_PERCPU_HPP
