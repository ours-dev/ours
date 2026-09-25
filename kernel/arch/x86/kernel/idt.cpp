#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/faults.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/arch/x86/idt.hpp>
#include <ours/arch/x86/percpu.hpp>
#include <ours/init.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vmm.hpp>

#include <arch/x86/descriptor.hpp>
#include <logz4/log.hpp>

namespace ours {
// We should migrate it to `lib/arch` in the future.
struct PACKED IdtEntry {
    auto set(usize handler) -> void {
        offset_low = handler & 0xFFFF;
        selector = X86_GDT_KERNEL_CODE64;
        ist = 0;
        type_attr = 0x8E; // Present, DPL=0 and INT gate
        offset_mid = (handler >> 16) & 0xFFFF;
        offset_high = (handler >> 32) & 0xFFFFFFFF;
        zero = 0;
    }

    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
};

struct Idt {
    auto load() const -> void {
        struct PACKED {
            u16 len;
            usize addr;
        } desc{sizeof(*this) - 1, reinterpret_cast<usize>(this)};
        asm volatile("lidt %0" ::"m"(desc));
    }

    auto begin() {
        return std::begin(entries_);
    }

    auto end() {
        return std::end(entries_);
    }

    IdtEntry entries_[256];
};

/// Encoded length of `clac` (0f 01 ca), the instruction every ISR stub opens
/// with. See the shift computed in x86_init_idt_early().
constexpr auto CLAC_SIZE = usize(3);

/// Turns a raw table slot into a real gate descriptor.
///
/// idt.S lays the table out as 256 pairs of `.quad <stub address>; .quad 0`,
/// so before this runs the first eight bytes of a slot are the handler and
/// the rest is zero. Reading the address back out of the slot it is about to
/// overwrite is why this takes the entry rather than an address.
///
/// `clac_shift` moves the gate past the stub's opening instruction; it is
/// zero whenever that instruction has to run.
FORCE_INLINE
static auto rectify_idt(IdtEntry &entry, usize clac_shift) -> void {
    entry.set(*reinterpret_cast<usize *>(&entry) + clac_shift);
}

/// Defined in arch/x86/main/idt.S, default value per entry is
/// the hanler.
extern "C" Idt IDT;
static Idt *PIDT = &IDT;

auto x86_load_idt() -> void {
    PIDT->load();
}

/// Makes |vec|'s gate switch to the stack published in TSS slot IST|slot|.
///
/// |slot| is the 1-based number the gate's ist field holds, where 0 means "no
/// switch": ist_slot() turns an IntrStackOrdering into it.
INIT_CODE
static auto x86_set_vector_ist(arch::IrqVec vec, u8 slot) -> void {
    auto const index = usize(vec);
    DEBUG_ASSERT(index < std::size(IDT.entries_), "Invalid vector: {}", index);
    DEBUG_ASSERT(slot >= 1 && slot <= X86Tss::NUM_IST_SLOTS, "Invalid IST slot: {}", slot);
    auto &entry = IDT.entries_[index];
    entry.ist = slot;

    log::info("Vector[{}] was bind to IST[{}]", index, slot);
}

INIT_CODE
auto x86_init_idt_early() -> void {
    // Every stub in idt.S opens with `clac`, unconditionally, and the comment
    // there says it must stay the first instruction. This is why: `clac` is an
    // SMAP instruction, so on a CPU without SMAP it raises #UD -- and since the
    // #UD stub opens with `clac` as well, the very first exception of any kind
    // would become an endless #UD, then #DF, then a triple fault, taking the
    // real fault down with it and leaving nothing to diagnose from.
    //
    // The fix is not to rewrite the text but to point every gate three bytes
    // past it. A stub is only ever entered through the IDT, so no other path
    // can reach the instruction being skipped, and no page has to be made
    // writable to do it. Skipping is also correct rather than merely expedient:
    // AC is what SMAP acts on, so with no SMAP there is nothing to clear.
    //
    // Zircon's idt_setup() does exactly this; ours cannot instead rely on
    // `.code-patches`, which this kernel does not build with
    // (ours_kernel_has_code_patches is false).
    //
    // x86_has_feature() runs its own CPUID, so this works at this point in
    // boot without depending on any feature cache being populated yet.
    auto const clac_shift = x86_has_feature(CpuFeatureType::Smap) ? usize(0) : CLAC_SIZE;
    for (auto &entry: IDT) {
        rectify_idt(entry, clac_shift);
    }

    // The three vectors that can arrive with %rsp unusable take a stack of
    // their own: #DF, when a kernel stack overflow leaves nowhere to push the
    // #PF; NMI and #MC, which can land at any instruction -- including
    // between SYSCALL and the stack swap in syscall.S, where %rsp is still
    // the user's. These used to pass the IntrStackOrdering straight in, which
    // numbers from 0, so #DF got ist = 0 -- "no switch" -- and NMI and #MC
    // named slots x86_setup_tss_percpu() never filled, switching to %rsp = 0.
    //
    // #DB keeps the thread's stack: DEBUG_BREAK_STACK is reserved but not
    // assigned, because deliver_user_exception() relies on every exception
    // frame living on the thread's kernel stack. Zircon's x86_init_percpu()
    // assigns the same three vectors (NMI_IST_INDEX and friends) and leaves
    // #DB alone too.
    x86_set_vector_ist(arch::IrqVec::DoubleFault, ist_slot(DOUBLE_FAULT_STACK));
    x86_set_vector_ist(arch::IrqVec::Nmi, ist_slot(NMI_STACK));
    x86_set_vector_ist(arch::IrqVec::MachineCheck, ist_slot(MACHINE_CHECK_STACK));

    // x86_dump_idt();
}

INIT_CODE
auto x86_setup_idt() -> void {
    using namespace mm;

    // Remap IDT to a readonly page
    auto rvma = VmAspace::kernel_aspace()->root_vma();
    DEBUG_ASSERT(rvma);
    auto result =
        rvma->map_phys(mm::virt_to_phys(VirtAddr(PIDT)), VmPlacement::anywhere(), PAGE_SIZE,
                       MmuFlags::Readable, VmMapOption::Commit | VmMapOption::Pinned, "k:IDT");
    ASSERT(result, "Failed to set readonly GDT");

    PIDT = reinterpret_cast<decltype(PIDT)>((*result)->base());
    x86_load_idt();
}

auto x86_dump_idt() -> void {
    log::info("IDT Dumpping Infomation");
    for (usize i = 0; i < std::size(IDT.entries_); ++i) {
        auto &entry = IDT.entries_[i];
        log::info("  [0]=(handler=0x{:X}, DPL={}, TYPE={})",
                  entry.offset_low | (entry.offset_mid << 16) | (u64(entry.offset_high) << 32),
                  (entry.type_attr >> 5) & 0x3, (entry.type_attr & 0xF) == 0xE ? "INTR" : "TRAP");
    }
}

} // namespace ours
