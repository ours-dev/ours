/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The kernel side of userboot: find the first user program, build a process
/// around it, and enter it.
///
/// This is being written a step at a time, and each step is committed only
/// once it has been seen to work in QEMU. Right now it does step one: locate
/// userboot's ELF inside the kernel package. Nothing is created yet.
#include <userabi/userboot.hpp>

#include <algorithm>

#include <cstring>

#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ours/vdso.hpp>
#include <ustl/mem/align.hpp>

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

    /// The program this loader would rather start: //examples/shell.
    ///
    /// userboot exists to prove the kernel can reach ring 3 and answer one
    /// call; once something more useful is in the package there is no reason to
    /// start the proof instead of the program. It stays a fallback rather than
    /// a replacement so that the kernel does not depend on //examples existing
    /// -- a tree built without it still boots to userboot.
    constexpr std::string_view PROGRAM_FILE{"shell"};

    /// The vDSO's name in the same BootFS, installed by
    /// //kernel/lib/userabi/vdso with the same has_patches = false convention.
    constexpr std::string_view VDSO_FILE{"vdso"};

    /// Decompresses the kernel package into a fresh buffer.
    ///
    /// |out| receives that buffer and the caller owns it. It is deliberately
    /// never freed: the bytes of every file in the package -- userboot's ELF
    /// among them -- are views *into* it, so releasing it would dangle every
    /// one of them, and it is wanted for as long as the kernel package is.
    ///
    /// The buffer used to come straight from the frame allocator, because the
    /// decompressed BootFS is megabytes and `operator new` answered nullptr
    /// past the 4 KiB size class. It routes to vmalloc now
    /// (docs/changes/260908/heap.md), so this is an ordinary allocation --
    /// and one that asks for the exact byte count rather than a run of frames
    /// rounded up to a power of two.
    auto materialize_kernel_package(u8 *&out) -> ustl::Result<phys::KernelPackage, Status> {
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

        out = new (mm::GAF_KERNEL) u8[bytes];
        if (!out) {
            log::error("userboot: could not allocate {} bytes for the kernel package", bytes);
            return ustl::err(Status::OutOfMem);
        }

        // Hands back a view over the decompressed BootFS. Everything the
        // package reports -- names, sizes, file bytes -- points into the
        // buffer just allocated.
        auto result = boot::kernel_package({out, bytes});
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

    /// What loading produced: how many bytes are mapped, where to start
    /// executing, and the top of the stack.
    ///
    /// An ordinary local now. It was a file-scope variable because the thread
    /// that entered ring 3 was a bare kernel thread reading its entry point
    /// out of a global -- nothing else could have carried the value from
    /// userboot_init() to a routine that runs later. ThreadDispatcher::start()
    /// takes the entry point as an argument and keeps it on the dispatcher, so
    /// there is nothing left to leave lying around.
    struct LoadedImage {
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

    /// Lays userboot's PT_LOAD segments out in fresh frames and maps them
    /// into a new user address space.
    ///
    /// userboot is a PIE (ET_DYN) with no relocations at all -- checked with
    /// llvm-readelf, and true because it references nothing outside itself
    /// -- so it can be laid down at any page-aligned base and simply run.
    /// That is what makes this short: no relocation pass, no dynamic
    /// linking, no symbol resolution.
    /// Where the vDSO goes.
    ///
    /// Between the image and the stack, at a fixed address for the same reason
    /// both of those are: there is no ASLR yet and nothing to collide with.
    /// It is the mapping most worth randomising once there is -- it is the
    /// only code in the address space that can enter the kernel -- so this
    /// constant is a placeholder for a per-process placement, not a decision
    /// that it should be fixed.
    constexpr VirtAddr VDSO_BASE = USER_ASPACE_BASE + MB(8);

    /// One mapped ELF image: where it landed, how much of the address space it
    /// covers, and what its e_entry resolved to.
    struct MappedImage {
        VirtAddr base = 0;
        usize span = 0;
        VirtAddr entry = 0;
    };

    /// Lays an ELF's PT_LOAD segments out in fresh frames and maps them into a
    /// user address space at |where|.
    ///
    /// Both images this kernel loads are PIEs (ET_DYN) with no relocations at
    /// all -- checked with llvm-readelf, and true because neither references
    /// anything outside itself -- so either can be laid down at any
    /// page-aligned base and simply run. That is what makes this short: no
    /// relocation pass, no dynamic linking, no symbol resolution.
    ///
    /// One routine for both, because they differ only in placement,
    /// permissions and name. The vDSO in particular must NOT be writable: it
    /// is the only code allowed to enter the kernel, so a program able to
    /// rewrite it could put a trap anywhere it liked inside the one range the
    /// kernel trusts.
    auto map_elf_image(ustl::views::Span<u8 const> bytes, object::VmaDispatcher &root_vma,
                       mm::VmPlacement where, arch::paging::MmuFlags flags, char const *name,
                       MappedImage &out) -> Status {
        if (bytes.size() < sizeof(Elf64Header)) {
            log::error("userboot: {} is too small to be an ELF", name);
            return Status::InvArgs;
        }

        Elf64Header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        // The segments' virtual addresses are relative to nothing in
        // particular for a PIE, so the span is measured from the lowest to the
        // highest address any PT_LOAD claims, and the whole thing is then
        // placed where the caller says. `memsz` and not `filesz`: the
        // difference between them is .bss, which occupies address space but
        // occupies no bytes in the file.
        auto lowest = ~VirtAddr(0);
        auto highest = VirtAddr(0);
        for (auto index = u16(0); index < header.phnum; ++index) {
            Elf64ProgramHeader phdr{};
            auto const at = header.phoff + usize(index) * header.phentsize;
            if (at + sizeof(phdr) > bytes.size()) {
                log::error("userboot: {} program header {} runs past the image", name, index);
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
            log::error("userboot: {} has no PT_LOAD segments", name);
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
            log::error("userboot: could not allocate {} frames for {}", nr_frames, name);
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
                log::error("userboot: {} segment {} content runs past the image", name, index);
                return Status::InvArgs;
            }
            std::memcpy(backing + (VirtAddr(phdr.vaddr) - lowest), bytes.data() + phdr.offset,
                        phdr.filesz);
        }

        // Two steps, deliberately: first say what the memory *is*, then say
        // where it appears. That split is the whole reason a VMO is a separate
        // object from a mapping -- it is what lets the same memory be mapped
        // twice, and what makes "hand a program some memory" a different
        // operation from "put it in that program's address space".
        //
        // A physical VMO, because these frames already exist and have already
        // been filled in. A paged VMO is the honest answer -- these are
        // ordinary anonymous pages, not MMIO -- and is what this becomes the
        // moment mm::VmObjectPaged::write() stops returning Unimplemented.
        object::KernelHandle<object::VmoDispatcher> vmo;
        Rights vmo_rights;
        auto const status = object::VmoDispatcher::create_physical(
            mm::PhysMap::virt_to_phys(backing), out.span, vmo, vmo_rights);
        if (Status::Ok != status) {
            log::error("userboot: could not create the {} VMO: {}", name, to_string(status));
            return status;
        }

        // Through the dispatcher, not through the VmArea it stands for: a
        // VmArea knows what a mapping is allowed to be, but cannot know who is
        // asking, and every future sys_vmar_map has to answer that. Rights
        // live on the handle, so the entry point has to be on the object that
        // a handle names.
        auto mapping =
            root_vma.map(*vmo.dispatcher(), where, out.span, flags, mm::VmMapOption::Commit, name);
        if (mapping.is_err()) {
            log::error("userboot: could not map {}: {}", name, to_string(mapping.unwrap_err()));
            return mapping.unwrap_err();
        }

        out.base = mapping.unwrap()->base();
        // e_entry is a file-relative address for a PIE, so it moves with the
        // load base like everything else. For userboot it is the first
        // instruction; for the vDSO it is the export table, which is the whole
        // reason that image is linked with -Wl,-e,OURS_VDSO_TABLE.
        out.entry = out.base + (VirtAddr(header.entry) - lowest);
        return Status::Ok;
    }

    /// Maps the first user program's image and gives it a stack.
    /// |name| is a plain char const * rather than a string_view because both
    /// the mapping and the process below want a C string, and both constants it
    /// is ever called with are literals -- so there is exactly one place
    /// (userboot_init) where .data() is taken, and it is provably terminated
    /// there rather than at each use.
    auto load_program(ustl::views::Span<u8 const> bytes, char const *name,
                      object::VmaDispatcher &root_vma, LoadedImage &out) -> Status {
        using arch::paging::MmuFlags;

        // MmuFlags::User is the whole point of this line: without it the pages
        // are supervisor-only and ring 3 faults on the first instruction
        // fetch. Writable as well as executable for now because the three
        // segments (R, RX, RW) are being mapped as one range; giving each its
        // own permissions is a later step, and one worth taking -- an image
        // that is writable and executable at once is exactly what W^X exists
        // to forbid.
        //
        // Note it does not defeat the vDSO rule: a program that writes a
        // `syscall` into its own text still traps from an address outside the
        // vDSO, which is exactly what the kernel refuses.
        constexpr auto IMAGE_FLAGS =
            MmuFlags::User | MmuFlags::Readable | MmuFlags::Writable | MmuFlags::Executable;

        MappedImage image;
        auto status = map_elf_image(bytes, root_vma, mm::VmPlacement::at(USER_ASPACE_BASE),
                                    IMAGE_FLAGS, name, image);
        if (Status::Ok != status) {
            return status;
        }
        out.span = image.span;
        out.entry = image.entry;

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
        return Status::Ok;
    }

    /// Maps the vDSO and validates the export table it publishes.
    ///
    /// Read-only and executable, never writable: this is the only code in the
    /// address space allowed to enter the kernel, so a program that could
    /// rewrite it could place a trap anywhere inside the one range the kernel
    /// trusts.
    auto load_vdso(ustl::views::Span<u8 const> bytes, object::VmaDispatcher &root_vma,
                   MappedImage &out) -> Status {
        using arch::paging::MmuFlags;
        constexpr auto VDSO_FLAGS = MmuFlags::User | MmuFlags::Readable | MmuFlags::Executable;

        auto const status =
            map_elf_image(bytes, root_vma, mm::VmPlacement::at(VDSO_BASE), VDSO_FLAGS, "vdso", out);
        if (Status::Ok != status) {
            return status;
        }

        // The table is read through the kernel's own view of the frames rather
        // than through the user mapping: the mapping is not this address
        // space, and reading a user address from here would be reading
        // whatever the *current* address space has there.
        auto const table_offset = out.entry - out.base;
        if (table_offset + sizeof(VdsoEntryTable) > out.span) {
            log::error("userboot: the vDSO entry table runs past the image");
            return Status::InvArgs;
        }
        VdsoEntryTable table{};
        std::memcpy(&table, bytes.data() + table_offset, sizeof(table));

        // Checked, because e_entry is only a promise. A vDSO built from a
        // different tree, or one whose table moved, would otherwise be called
        // into as an indirect jump to whatever happens to be there -- in ring
        // 3, with no way to report what went wrong.
        if (table.magic != OURS_VDSO_MAGIC) {
            log::error("userboot: vDSO entry table magic is {:X}, expected {:X}", table.magic,
                       u32{OURS_VDSO_MAGIC});
            return Status::BadType;
        }
        if (table.version != OURS_VDSO_VERSION) {
            log::error("userboot: vDSO ABI version {} does not match the kernel's {}",
                       table.version, u16{OURS_VDSO_VERSION});
            return Status::Unsupported;
        }

        log::info("userboot: vdso mapped [{:X}, {:X}), table at {:X}, {} entries", out.base,
                  out.base + out.span, out.entry, table.nr_entries);
        return Status::Ok;
    }
} // namespace

auto userboot_init() -> void {
    u8 *package_buffer = nullptr;
    auto const package = materialize_kernel_package(package_buffer);
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

    auto program = PROGRAM_FILE;
    auto image = package.unwrap().file(program);
    if (image.is_err()) {
        // Not an error yet: a tree built without //examples has no shell, and
        // the proof-of-life program is still there to start.
        log::info("userboot: no '{}' in the kernel package, falling back to '{}'", program,
                  USERBOOT_FILE);
        program = USERBOOT_FILE;
        image = package.unwrap().file(program);
    }
    if (image.is_err()) {
        log::error("userboot: '{}' is not in the kernel package: {}", program,
                   to_string(image.unwrap_err()));
        return;
    }

    log::info("userboot: found '{}', {} bytes of ELF", program, image.unwrap().size());

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
        object::ProcessDispatcher::create(object::JobDispatcher::root_job(), program.data(),
                                          process, process_rights, root_vma, root_vma_rights);
    if (Status::Ok != status) {
        log::error("userboot: could not create the process: {}", to_string(status));
        return;
    }

    LoadedImage loaded;
    // .data() is a C string here: both PROGRAM_FILE and USERBOOT_FILE are
    // constexpr views over string literals, so the terminator is the literal's.
    if (Status::Ok != load_program(image.unwrap(), program.data(), *root_vma.dispatcher(), loaded)) {
        return;
    }

    log::info("userboot: mapped [{:X}, {:X}) into its own aspace, entry at {:X}, stack {:X}",
              USER_ASPACE_BASE, USER_ASPACE_BASE + loaded.span, loaded.entry, loaded.stack_top);

    // The vDSO, mapped into the same address space. It is what the program
    // enters the kernel through, and after the next commit it is the only
    // thing that can: the syscall path refuses a trap whose return address
    // lies outside the range recorded below.
    auto const vdso_bytes = package.unwrap().file(VDSO_FILE);
    if (vdso_bytes.is_err()) {
        log::error("userboot: '{}' is not in the kernel package: {}", VDSO_FILE,
                   to_string(vdso_bytes.unwrap_err()));
        return;
    }

    MappedImage vdso;
    if (Status::Ok != load_vdso(vdso_bytes.unwrap(), *root_vma.dispatcher(), vdso)) {
        return;
    }

    // Recorded on the process, not in a global: the range is a property of one
    // address space, and the syscall path reaches it through the thread that
    // trapped. One process today, but a global would be a lie the moment there
    // are two, and it would be found the hard way.
    process.dispatcher()->set_vdso_code_range(vdso.base, vdso.span);

    // The thread comes from the object layer too, which is what retires the
    // last three lines this function used to end with: task::Thread::spawn(),
    // a hand-built interrupt frame in a routine of its own, and
    // Thread::set_aspace(). ThreadDispatcher::spawn() does the aspace wiring
    // (it knows the process), and its trampoline builds the frame that enters
    // ring 3 -- the same code, reached through the object that owns it.
    object::KernelHandle<object::ThreadDispatcher> thread;
    status = object::ThreadDispatcher::spawn(process.dispatcher(), "userboot", &thread);
    if (Status::Ok != status) {
        log::error("userboot: could not create the entry thread: {}", to_string(status));
        return;
    }

    // start(), not resume(): where a kernel thread only needs to be made
    // runnable, a user thread has to be told where in user space to begin.
    // This is the same argument list a future sys_thread_start carries.
    // arg1 is the vDSO's export table, which is the only thing the program is
    // told about the world it wakes up in. It lands in %rdi
    // (arch_build_uspace_irqframe), so `_start` simply takes it as its first
    // parameter -- no dynamic linker, no symbol lookup, no fixed address baked
    // into the binary.
    status = thread.dispatcher()->start({
        .entry = loaded.entry,
        .stack = loaded.stack_top,
        .arg1 = vdso.entry,
        .arg2 = 0,
    });
    if (Status::Ok != status) {
        log::error("userboot: could not start the entry thread: {}", to_string(status));
        return;
    }

    // Every handle in this function goes out of scope here, and nothing leaks:
    // the kernel thread holds the ThreadDispatcher, which holds the
    // ProcessDispatcher, which owns the address space the mappings live in.
    // The VMA and VMO dispatchers were the objects that were *allowed* to
    // create those mappings, not the things that hold them.
}
} // namespace userabi
