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
/// One of the dedicated stacks an IST gate switches to (IntrStackOrdering).
///
/// base() and top() mean what they mean on mm::Stack: base() is the lowest
/// byte, top() one past the highest -- the value a downward-growing stack
/// starts from, and so the value a TSS IST slot holds. They used to be named
/// the other way round (top() returned the lowest byte), which is harmless
/// only as long as nothing loads one into %rsp.
struct InterruptStack {
    constexpr static auto INTR_STACK_SIZE = KB(16);

    FORCE_INLINE
    auto base() -> VirtAddr {
        return reinterpret_cast<VirtAddr>(stack);
    }

    FORCE_INLINE
    auto top() -> VirtAddr {
        return reinterpret_cast<VirtAddr>(stack + INTR_STACK_SIZE);
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

/// The TSS slot a dedicated stack is published in, as the IDT gate's ist
/// field names it.
///
/// The two numberings differ by one and this is the only place they meet.
/// IntrStackOrdering counts from 0 because it indexes X86PerCpu::intr_stack;
/// the gate's ist field counts from 1 because 0 there means "stay on the
/// current stack". Handing the ordering to the gate directly -- which
/// x86_init_idt_early() used to do -- quietly gave #DF, ordering 0, no stack
/// switch at all.
constexpr auto ist_slot(IntrStackOrdering which) -> u8 {
    return static_cast<u8>(which + 1);
}

struct PACKED X86Tss {
    typedef X86Tss Self;

    /// How many ports the I/O permission bitmap has a bit for: every port
    /// there is, 0 to 0xFFFF.
    constexpr static auto NUM_IO_PORTS = static_cast<u32>(arch::MAX_IO_BITMAP_BYTES * 8);

    /// Points the CPU at the I/O permission bitmap below and closes every
    /// port in it.
    ///
    /// Ring 3 runs at IOPL 0, so its in/out/ins/outs are decided by this
    /// bitmap: a set bit refuses the port with #GP, a clear bit lets the
    /// access through without the kernel seeing anything. The bitmap used to
    /// be left as the zero-initialised storage gave it -- every port open --
    /// so any program could reset the machine through port 0x64 or 0xCF9,
    /// rewrite PCI configuration through 0xCF8/0xCFC, or program the PIT and
    /// the PIC, and none of it was a fault the kernel could kill it for.
    /// Every bit set here is every port closed. The ports a process opened
    /// with ioports_request are cleared again by IoBitmap::load()
    /// (io_bitmap.hpp) while one of its threads runs on this CPU, and set
    /// once another process's thread takes the CPU over.
    ///
    /// The byte after the bitmap is all ones as well, as the architecture
    /// requires: an access that straddles the last byte reads one past it.
    ///
    /// Called before ltr, from x86_setup_tss_percpu() (descriptor.cpp).
    FORCE_INLINE
    auto init() -> void {
        io_port_bitmap_base = static_cast<u16>(ustl::mem::offset_of(&Self::io_bitmap));
        close_all_ports();
    }

    /// Lets ring 3 use the ports [first, end) on this CPU: clears their bits.
    auto open_ports(u32 const first, u32 const end) -> void {
        mark_ports(first, end, false);
    }

    /// Refuses the ports [first, end) to ring 3 on this CPU again: sets their
    /// bits.
    auto close_ports(u32 const first, u32 const end) -> void {
        mark_ports(first, end, true);
    }

    /// Refuses every port, as init() leaves the bitmap: all 8 KiB of it, and
    /// the byte after it, which has to stay all ones anyway.
    auto close_all_ports() -> void {
        for (auto &byte: io_bitmap) {
            byte = 0xff;
        }
    }

    /// Whether ring 3 may use |port| on this CPU: its bit is clear.
    [[nodiscard]]
    auto port_is_open(u32 const port) const -> bool {
        return (io_bitmap[port / 8] & (1U << (port % 8))) == 0;
    }

    /// Publishes |top| as the stack pointer the CPU loads for a gate whose
    /// ist field is |slot| -- the SDM's IST1..IST7, hence 1-based.
    FORCE_INLINE
    auto set_ist(u8 slot, VirtAddr top) -> void {
        DEBUG_ASSERT(slot >= 1 && slot <= NUM_IST_SLOTS, "Invalid IST slot: {}", slot);
        ist[slot - 1] = top;
    }

    constexpr static u8 NUM_IST_SLOTS = 7;

    // We hope to use two views to access the bitmap.
    u32 reserved0{};
    u64 sp0{};
    u64 sp1{};
    u64 sp2{};
    u64 reserved1{};
    /// IST1..IST7, one array so that set_ist() can index it: ist[n - 1] is
    /// the SDM's ISTn.
    u64 ist[NUM_IST_SLOTS]{};
    u64 reserved2{};
    u16 reserved3{};
    u16 io_port_bitmap_base{};
    u8 io_bitmap[arch::MAX_IO_BITMAP_BYTES + 1]{};

  private:
    /// Sets the bits of the ports [first, end) when |closed|, clears them
    /// otherwise: the partial bytes at either end a bit at a time, the whole
    /// bytes between them a byte at a time.
    auto mark_ports(u32 const first, u32 const end, bool const closed) -> void {
        DEBUG_ASSERT(first <= end && end <= NUM_IO_PORTS, "ports [{:#x}, {:#x}) past the bitmap",
                     first, end);
        auto fill = u8{0x00};
        if (closed) {
            fill = 0xff;
        }
        auto port = first;
        for (; port < end && port % 8 != 0; ++port) {
            mark_port(port, closed);
        }
        for (; end - port >= 8; port += 8) {
            io_bitmap[port / 8] = fill;
        }
        for (; port < end; ++port) {
            mark_port(port, closed);
        }
    }

    auto mark_port(u32 const port, bool const closed) -> void {
        auto const bit = static_cast<u8>(1U << (port % 8));
        if (closed) {
            io_bitmap[port / 8] |= bit;
        } else {
            io_bitmap[port / 8] &= static_cast<u8>(~bit);
        }
    }
};
static_assert(offsetof(X86Tss, sp0) == 4);
static_assert(offsetof(X86Tss, sp2) == 20);
static_assert(offsetof(X86Tss, ist) == 0x24, "IST1 sits at byte 0x24 of a 64-bit TSS");
static_assert(ist_slot(static_cast<IntrStackOrdering>(MAX_NUM_STACKS - 1)) <=
              X86Tss::NUM_IST_SLOTS);

struct alignas(16) X86PerCpu {
    typedef X86PerCpu Self;

    X86Tss tss;
    u32 apic_id;
    u32 padding_;
    CpuNum cpunum;
    InterruptStack intr_stack[MAX_NUM_STACKS];
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
