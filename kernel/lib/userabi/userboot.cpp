/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The kernel side of userboot: find the first user program, build a process
/// around it, and enter it.
///
/// This is being written a step at a time, and each step is committed only
/// once it has been seen to work in QEMU. Right now it does step one: locate
/// userboot's ELF inside the kernel package. Nothing is created yet.
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/entry.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/task/thread.hpp>
#include <ustl/mem/align.hpp>

#include <algorithm>

#include <cstring>

#include <userabi/userboot.hpp>

namespace userabi {
using namespace ours;

namespace {
    /// The name userboot's ELF has inside the kernel package's BootFS.
    ///
    /// Not "userboot/image.elf". `kernel_elf_binary("userboot")` in
    /// //kernel/lib/userabi/userboot/BUILD.gn sets `has_patches = false`,
    /// and kernel_elf_image() installs a patch-less binary as one bare file
    /// rather than as a `<name>/` subtree -- which is why vmours, which does
    /// have patches, is "vmours/image.elf" instead. The two live in the same
    /// BootFS with different shapes, so this cannot be derived from the
    /// kernel's own name; it has to be stated.
    constexpr std::string_view USERBOOT_FILE{"userboot"};

    /// The decompressed kernel package, and the pages holding it.
    ///
    /// This is deliberately not a `ktl::Vec<u8>`. ktl's allocator is
    /// kmalloc, whose largest size class is 4 KiB, and an oversized request
    /// there answers nullptr rather than falling back to pages; Vec then
    /// constructs elements at that nullptr and page-faults inside reserve().
    /// The decompressed BootFS is megabytes -- it carries the whole kernel
    /// ELF -- so it has to come from the frame allocator and be read through
    /// the physmap. //kernel/commands/prof.cpp hit exactly this and its
    /// FdeTable carries the same note.
    struct Package {
        u8 *base = nullptr;
        usize nr_frames = 0;
    };

    /// Decompresses the kernel package into freshly allocated frames.
    ///
    /// The caller owns the frames. They are not freed here because the
    /// bytes of every file in the package -- userboot's ELF among them --
    /// are views *into* this buffer: dropping it would dangle them.
    auto materialize_kernel_package(Package &out) -> ustl::Result<phys::KernelPackage, Status> {
        // The size has to be known before the buffer can exist, and only the
        // OBI item header knows it: `memory_size` is the *decompressed*
        // length, as against the compressed length stored in the container.
        // BootObi::kernel_package_item() exists for precisely this two-step
        // (its own comment says so) -- the one-shot boot::kernel_package()
        // is for callers that already have a big enough buffer.
        auto const package = boot::boot_package();
        if (package.is_err()) {
            log::error("userboot: no boot package: {}", to_string(package.unwrap_err()));
            return ustl::err(package.unwrap_err());
        }

        auto const item = package.unwrap().kernel_package_item();
        if (item.is_err()) {
            log::error("userboot: no kernel package item: {}", to_string(item.unwrap_err()));
            return ustl::err(item.unwrap_err());
        }

        auto const bytes = static_cast<usize>(item.unwrap().memory_size());

        // The frame allocator deals in whole pages and, through get_order(),
        // in power-of-two runs of them, so round up rather than asking for a
        // byte count it cannot express.
        out.nr_frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        out.base = mm::get_frames_n<u8>(mm::GAF_KERNEL, out.nr_frames);
        if (!out.base) {
            log::error("userboot: could not allocate {} frames for the kernel package",
                       out.nr_frames);
            return ustl::err(Status::OutOfMem);
        }

        // Hands back a view over the decompressed BootFS. Everything the
        // package reports -- names, sizes, file bytes -- points into the
        // frames just allocated.
        auto result = boot::kernel_package({out.base, bytes});
        if (result.is_err()) {
            log::error("userboot: could not materialize the kernel package: {}",
                       to_string(result.unwrap_err()));
            return ustl::err(result.unwrap_err());
        }

        return ustl::ok(result.unwrap());
    }

    /// The 64-bit ELF header and program header, as far as loading needs
    /// them.
    ///
    /// Declared here rather than reached for from //kernel/lib/eftl or
    /// ours::phys::ElfImage. eftl is built for dynamic linking and reaches
    /// segments through a variant of traits classes; ElfImage has exactly
    /// the load plan wanted, but lives in phys's `physload.core` source_set
    /// and is not linked into the kernel. Loading one static PIE is two
    /// dozen lines of arithmetic, and having those lines visible is worth
    /// more here than the reuse would be.
    struct [[gnu::packed]] Elf64Header {
        u8 ident[16];
        u16 type;
        u16 machine;
        u32 version;
        u64 entry;
        u64 phoff;
        u64 shoff;
        u32 flags;
        u16 ehsize;
        u16 phentsize;
        u16 phnum;
        u16 shentsize;
        u16 shnum;
        u16 shstrndx;
    };

    struct [[gnu::packed]] Elf64ProgramHeader {
        u32 type;
        u32 flags;
        u64 offset;
        u64 vaddr;
        u64 paddr;
        u64 filesz;
        u64 memsz;
        u64 align;
    };

    constexpr u32 PT_LOAD = 1;

    /// What loading produced: the process userboot was built into, how many
    /// bytes of it are mapped, where to start executing, and the top of its
    /// stack.
    ///
    /// The process, and not the address space it runs in. Holding the aspace
    /// directly is how this used to be written, and it is exactly the shortcut
    /// this file is meant not to take: kernel/object is the layer of kernel
    /// objects that serve users, and everything below it -- VmAspace, VmArea,
    /// task::Thread -- serves the kernel itself. userboot is the first *user*
    /// program, so the path that builds it has to be the path a system call
    /// would take on userspace's behalf. The address space is still reachable,
    /// through the process that owns it.
    struct LoadedImage {
        object::KernelHandle<object::ProcessDispatcher> process;
        usize span = 0;
        VirtAddr entry = 0;
        VirtAddr stack_top = 0;
    };

    /// Where userboot's stack goes, and how big.
    ///
    /// Placed a long way above the image rather than immediately after it, so
    /// that a stack overflow runs into unmapped address space and faults
    /// instead of quietly chewing through .bss. One page is plenty for a
    /// program whose whole text is a spin loop; it is a fixed address only
    /// because there is no ASLR and nothing else to collide with yet.
    constexpr VirtAddr USER_STACK_TOP = USER_ASPACE_BASE + MB(16);
    constexpr usize USER_STACK_SIZE = PAGE_SIZE;

    /// The image, kept alive past userboot_init(): the thread that enters it
    /// is only spawned there and runs later, so nothing on that stack frame
    /// can be what it reads.
    LoadedImage USERBOOT_IMAGE;

    /// Lays userboot's PT_LOAD segments out in fresh frames and maps them
    /// into a new user address space.
    ///
    /// userboot is a PIE (ET_DYN) with no relocations at all -- checked with
    /// llvm-readelf, and true because it references nothing outside itself
    /// -- so it can be laid down at any page-aligned base and simply run.
    /// That is what makes this short: no relocation pass, no dynamic
    /// linking, no symbol resolution.
    auto load_image(ustl::views::Span<u8 const> bytes, object::VmaDispatcher &root_vma,
                    LoadedImage &out) -> Status {
        if (bytes.size() < sizeof(Elf64Header)) {
            log::error("userboot: image is too small to be an ELF");
            return Status::InvArgs;
        }

        Elf64Header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        // The segments' virtual addresses are relative to nothing in
        // particular for a PIE, so the span is measured from the lowest to
        // the highest address any PT_LOAD claims, and the whole thing is
        // then placed at USER_ASPACE_BASE. `memsz` and not `filesz`: the
        // difference between them is .bss, which occupies address space but
        // occupies no bytes in the file.
        auto lowest = ~VirtAddr(0);
        auto highest = VirtAddr(0);
        for (auto index = u16(0); index < header.phnum; ++index) {
            Elf64ProgramHeader phdr{};
            auto const at = header.phoff + usize(index) * header.phentsize;
            if (at + sizeof(phdr) > bytes.size()) {
                log::error("userboot: program header {} runs past the image", index);
                return Status::InvArgs;
            }
            std::memcpy(&phdr, bytes.data() + at, sizeof(phdr));
            if (phdr.type != PT_LOAD) {
                continue;
            }
            lowest = std::min(lowest, VirtAddr(phdr.vaddr));
            highest = std::max(highest, VirtAddr(phdr.vaddr + phdr.memsz));
        }

        if (lowest > highest) {
            log::error("userboot: no PT_LOAD segments");
            return Status::InvArgs;
        }

        // Whole pages: the frame allocator deals in nothing smaller, and a
        // mapping cannot start or end mid-page either.
        lowest = ustl::mm::align_down(lowest, PAGE_SIZE);
        highest = ustl::mm::align_up(highest, PAGE_SIZE);
        out.span = highest - lowest;

        auto const nr_frames = out.span / PAGE_SIZE;
        auto *const backing = mm::get_frames_n<u8>(mm::GAF_USER, nr_frames);
        if (!backing) {
            log::error("userboot: could not allocate {} frames for the image", nr_frames);
            return Status::OutOfMem;
        }

        // Zero first, then copy each segment's file bytes over the top. That
        // is what gives .bss its zeroes: the gap between filesz and memsz is
        // never written again, so it keeps the zeroes from here.
        std::memset(backing, 0, out.span);
        for (auto index = u16(0); index < header.phnum; ++index) {
            Elf64ProgramHeader phdr{};
            std::memcpy(&phdr, bytes.data() + header.phoff + usize(index) * header.phentsize,
                        sizeof(phdr));
            if (phdr.type != PT_LOAD || phdr.filesz == 0) {
                continue;
            }
            if (phdr.offset + phdr.filesz > bytes.size()) {
                log::error("userboot: segment {} content runs past the image", index);
                return Status::InvArgs;
            }
            std::memcpy(backing + (VirtAddr(phdr.vaddr) - lowest), bytes.data() + phdr.offset,
                        phdr.filesz);
        }

        // MmuFlags::User is the whole point of this line: without it the
        // pages are supervisor-only and ring 3 faults on the first
        // instruction fetch. Writable as well as executable for now because
        // the three segments (R, RX, RW) are being mapped as one range;
        // giving each its own permissions is a later step, and one worth
        // taking -- an image that is writable and executable at once is
        // exactly what W^X exists to forbid.
        using arch::paging::MmuFlags;
        auto const flags =
            MmuFlags::User | MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Executable;

        // Two steps, deliberately: first say what the memory *is*, then say
        // where it appears. That split is the whole reason a VMO is a separate
        // object from a mapping -- it is what lets the same memory be mapped
        // twice, and what makes "hand a program some memory" a different
        // operation from "put it in that program's address space".
        //
        // A physical VMO, because these frames already exist: get_frames_n()
        // above allocated them and the segments have already been copied in.
        // A paged VMO is the honest answer -- the image is ordinary anonymous
        // memory, not MMIO -- and is what this becomes the moment
        // mm::VmObjectPaged::write() stops returning Unimplemented. There is
        // no other way to get bytes into a paged VMO today.
        object::KernelHandle<object::VmoDispatcher> image_vmo;
        Rights image_vmo_rights;
        auto status = object::VmoDispatcher::create_physical(mm::PhysMap::virt_to_phys(backing),
                                                             out.span, image_vmo, image_vmo_rights);
        if (Status::Ok != status) {
            log::error("userboot: could not create the image VMO: {}", to_string(status));
            return status;
        }

        // Through the dispatcher, not through the VmArea it stands for: a
        // VmArea knows what a mapping is allowed to be, but cannot know who is
        // asking, and every future sys_vmar_map has to answer that. Rights
        // live on the handle, so the entry point has to be on the object that
        // a handle names.
        auto mapping = root_vma.map(*image_vmo.dispatcher(), mm::VmPlacement::at(USER_ASPACE_BASE),
                                    out.span, flags, mm::VmMapOption::Commit, "userboot");
        if (mapping.is_err()) {
            log::error("userboot: could not map the image: {}", to_string(mapping.unwrap_err()));
            return mapping.unwrap_err();
        }

        // A stack, backed by a paged VMO: unlike the image there is nothing to
        // copy into it, so zero-filled anonymous pages are exactly right and
        // no write() is needed. No Executable bit -- a stack that can be
        // executed is the classic way a buffer overflow becomes code
        // execution.
        object::KernelHandle<object::VmoDispatcher> stack_vmo;
        Rights stack_vmo_rights;
        status = object::VmoDispatcher::create_paged(USER_STACK_SIZE, stack_vmo, stack_vmo_rights);
        if (Status::Ok != status) {
            log::error("userboot: could not create the stack VMO: {}", to_string(status));
            return status;
        }

        auto stack = root_vma.map(
            *stack_vmo.dispatcher(), mm::VmPlacement::at(USER_STACK_TOP - USER_STACK_SIZE),
            USER_STACK_SIZE, MmuFlags::User | MmuFlags::Readable | MmuFlags::Writable,
            mm::VmMapOption::Commit, "userboot-stack");
        if (stack.is_err()) {
            log::error("userboot: could not map a stack: {}", to_string(stack.unwrap_err()));
            return stack.unwrap_err();
        }

        // Grows down, so the thread starts at the top. Sixteen-byte aligned
        // because the x86-64 ABI requires it at a function boundary, and the
        // top of a page already is.
        out.stack_top = USER_STACK_TOP;

        // The entry point is a file-relative address for a PIE, so it moves
        // with the load base like everything else.
        out.entry = USER_ASPACE_BASE + (VirtAddr(header.entry) - lowest);
        return Status::Ok;
    }
    /// Runs as an ordinary kernel thread, and stops being one.
    ///
    /// It never returns: x86_enter_uspace() ends in `iret` onto a fabricated
    /// interrupt frame, which is how ring 3 is reached at all -- there is no
    /// instruction that simply lowers the privilege level. From here on this
    /// thread's kernel stack is only re-entered through an interrupt or a
    /// syscall.
    ///
    /// The address space is not switched here. It was installed on the thread
    /// before it was resumed, so Scheduler::switch_context() has already
    /// loaded the right CR3 on the way in -- doing it again by hand would
    /// leave the scheduler's idea of the current aspace disagreeing with the
    /// hardware's.
    [[noreturn]]
    auto enter_userboot(void *) -> i32 {
        auto const &image = USERBOOT_IMAGE;

        arch::IrqFrame frame{};
        arch_build_uspace_irqframe(&frame, image.entry, image.stack_top, 0, 0);

        log::info("userboot: entering ring 3 at {:X}, stack {:X}", image.entry, image.stack_top);
        arch_enter_uspace(&frame);
    }
} // namespace

auto userboot_init() -> void {
    Package package_frames;
    auto const package = materialize_kernel_package(package_frames);
    if (package.is_err()) {
        return;
    }

    // Listing the whole package rather than going straight for the one file
    // is worth the few lines: the names come from the GN packaging rules,
    // and a mismatch between what the build installs and what this looks up
    // is otherwise invisible -- find() just answers NotFound and says
    // nothing about what *is* there.
    auto const &bootfs = package.unwrap().bootfs();
    log::info("userboot: kernel package holds {} files", bootfs.file_count());
    for (auto index = u32(0); index < bootfs.file_count(); ++index) {
        log::info("userboot:   {} ({} bytes)", bootfs.file_name(index),
                  bootfs.file_data(index).size());
    }

    auto const image = package.unwrap().file(USERBOOT_FILE);
    if (image.is_err()) {
        log::error("userboot: '{}' is not in the kernel package: {}", USERBOOT_FILE,
                   to_string(image.unwrap_err()));
        return;
    }

    log::info("userboot: found '{}', {} bytes of ELF", USERBOOT_FILE, image.unwrap().size());

    // One call builds both the process and the only object that may map into
    // it. They come back together because neither is useful alone: a process
    // with no root VMA cannot be given any memory, and a VMA dispatcher with
    // no process names a region nothing will ever run in.
    //
    // The rights come back as separate values because that is where rights
    // live -- on the handle, not on the dispatcher. Nothing consumes them yet;
    // they become the arguments to Handle::make() once the handle table
    // exists, which is the step that lets userboot be handed its own objects
    // rather than merely be built out of them.
    object::KernelHandle<object::ProcessDispatcher> process;
    object::KernelHandle<object::VmaDispatcher> root_vma;
    Rights process_rights;
    Rights root_vma_rights;
    auto status =
        object::ProcessDispatcher::create(object::JobDispatcher::root_job(), "userboot", process,
                                          process_rights, root_vma, root_vma_rights);
    if (Status::Ok != status) {
        log::error("userboot: could not create the process: {}", to_string(status));
        return;
    }

    auto &loaded = USERBOOT_IMAGE;
    if (Status::Ok != load_image(image.unwrap(), *root_vma.dispatcher(), loaded)) {
        return;
    }
    loaded.process = ustl::move(process);

    log::info("userboot: mapped [{:X}, {:X}) into its own aspace, entry at {:X}", USER_ASPACE_BASE,
              USER_ASPACE_BASE + loaded.span, loaded.entry);

    auto *const thread =
        task::Thread::spawn("userboot", &enter_userboot, nullptr, task::DEFAULT_PRIORITY);
    if (!thread) {
        log::error("userboot: could not spawn the entry thread");
        return;
    }

    // Before resume(), not after: the scheduler reads Thread::aspace() when
    // it switches to a thread, so the CR3 change and the thread's first
    // instruction have to be decided in that order. Setting it afterwards
    // would race the thread's own first run.
    //
    // Reached through the process rather than kept alongside it, because the
    // process is what owns the address space now. This hand-wiring is itself
    // the next thing to go: task::Thread already asks
    // m_user_thread->process().aspace() for exactly this value when it has a
    // ThreadDispatcher (thread.cpp, Thread::Current::fault), so once userboot
    // starts its thread through ThreadDispatcher there is nothing left to set
    // by hand here.
    thread->set_aspace(loaded.process.dispatcher()->aspace());
    thread->detach();
    thread->resume();

    // root_vma goes out of scope here, and with it the only reference to the
    // VMA dispatcher. That is correct: the mappings it made live in the
    // address space, which the process owns -- the dispatcher was the object
    // that was allowed to make them, not the thing that holds them.
}
} // namespace userabi
