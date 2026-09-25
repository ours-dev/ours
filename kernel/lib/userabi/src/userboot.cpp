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

#include <elfload/binder.hpp>
#include <elfload/loader.hpp>
#include <elfload/program.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/boot/options.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/status.hpp>
#include <ustl/mem/align.hpp>

namespace userabi {
using namespace ours;

namespace {
    /// The vDSO's name in the same BootFS, installed by
    /// //kernel/lib/userabi/vdso with the same has_patches = false convention.
    constexpr std::string_view VDSO_FILE{"vdso"};

    /// A program name, kept as a NUL-terminated buffer rather than as a view.
    ///
    /// launch() is handed a view -- from the boot options for the first
    /// program, from a user buffer for process_spawn_boot -- while
    /// ProcessDispatcher and the loader below both want a C string; copying
    /// once here is cheaper than having each caller find that out. The
    /// capacity is the one boot::BootOptions gives `kernel.select.userboot`,
    /// so any name the command line can select also fits here.
    struct ProgramName {
        char text[64]{};

        [[nodiscard]]
        auto view() const -> std::string_view {
            return {text};
        }
    };

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

    /// What loading produced: the mapped image, and the top of the stack that
    /// was mapped beside it.
    ///
    /// An ordinary local now. It was a file-scope variable because the thread
    /// that entered ring 3 was a bare kernel thread reading its entry point
    /// out of a global -- nothing else could have carried the value from
    /// userboot_init() to a routine that runs later. ThreadDispatcher::start()
    /// takes the entry point as an argument and keeps it on the dispatcher, so
    /// there is nothing left to leave lying around.
    struct LoadedImage {
        elfload::Image image;
        VirtAddr stack_top = 0;
    };

    /// Where a program's stack goes, and how big.
    ///
    /// Placed a long way above the image rather than immediately after it, so
    /// that a stack overflow runs into unmapped address space and faults
    /// instead of quietly chewing through .bss -- nothing is ever mapped below
    /// it, so that address space is the guard. 64 KiB rather than the one page
    /// it was: that was enough for a program whose whole text was a spin
    /// loop, and it is not enough for one that keeps a 2 KiB registry buffer
    /// and calls through several layers of ou. The address is fixed only
    /// because there is no ASLR yet; every process has its own address space,
    /// so two programs do not collide there.
    constexpr VirtAddr USER_STACK_TOP = USER_ASPACE_BASE + MB(16);
    constexpr usize USER_STACK_SIZE = KB(64);

    /// The stack pointer a program starts with, given the top of its stack.
    ///
    /// Not the top itself. The SysV ABI promises every function that at its
    /// entry %rsp + 8 is 16-byte aligned -- the call that got there pushed an
    /// 8-byte return address onto an aligned stack. `_start` is written as an
    /// ordinary C++ function and compiled on that promise, but nothing calls
    /// it: the kernel iretq's straight to it. Starting it at the aligned top
    /// left it misaligned by 8 for its whole life; nothing broke only because
    /// user code is built -mno-sse and so never spills an aligned 16-byte
    /// value. Zircon's compute_initial_stack_pointer() does the same.
    [[nodiscard]]
    constexpr auto initial_stack_pointer(VirtAddr const top) -> VirtAddr {
        return (top & ~VirtAddr{15}) - 8;
    }
    static_assert(initial_stack_pointer(USER_STACK_TOP) % 16 == 8);

    /// The kernel package, decompressed once at boot and kept.
    ///
    /// Every program started later -- the first one by userboot_init(),
    /// others through launch() on a user process's behalf -- comes out of the
    /// same package, and decompressing it again for each would cost megabytes
    /// every time. It used to be a local of userboot_init(), so nothing after
    /// the first program could reach it. Written once, before any user thread
    /// exists, and only read after that.
    ustl::Option<phys::KernelPackage> resident_package;

    /// Where the vDSO goes.
    ///
    /// Between the image and the stack, at a fixed address for the same reason
    /// both of those are: there is no ASLR yet and nothing to collide with.
    /// It is the mapping most worth randomising once there is -- it is the
    /// only code in the address space that can enter the kernel -- so this
    /// constant is a placeholder for a per-process placement, not a decision
    /// that it should be fixed.
    constexpr VirtAddr VDSO_BASE = USER_ASPACE_BASE + MB(8);

    /// Maps the first user program's image and gives it a stack.
    /// |name| is a plain char const * rather than a string_view because both
    /// the mapping and the process below want a C string, and both constants it
    /// is ever called with are literals -- so there is exactly one place
    /// (userboot_init) where .data() is taken, and it is provably terminated
    /// there rather than at each use.
    auto load_program(ustl::views::Span<u8 const> bytes, char const *name,
                      elfload::Loader const &loader, object::VmaDispatcher &root_vma,
                      LoadedImage &out) -> Status {
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

        auto status =
            loader.map(bytes, mm::VmPlacement::at(USER_ASPACE_BASE), IMAGE_FLAGS, name, out.image);
        if (Status::Ok != status) {
            return status;
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
        return Status::Ok;
    }

    /// Maps the vDSO, which is the image every user program's imports are
    /// bound to.
    ///
    /// Read-only and executable, never writable: this is the only code in the
    /// address space allowed to enter the kernel, so a program that could
    /// rewrite it could place a trap anywhere inside the one range the kernel
    /// trusts.
    auto load_vdso(ustl::views::Span<u8 const> bytes, elfload::Loader const &loader,
                   elfload::Image &out) -> Status {
        using arch::paging::MmuFlags;
        constexpr auto VDSO_FLAGS = MmuFlags::User | MmuFlags::Readable | MmuFlags::Executable;

        auto const status =
            loader.map(bytes, mm::VmPlacement::at(VDSO_BASE), VDSO_FLAGS, "vdso", out);
        if (Status::Ok != status) {
            return status;
        }

        // No magic or version check any more, and nothing is lost by that.
        // There used to be an export table at e_entry carrying both, because
        // the program was about to take a u32 offset out of it and call
        // through it -- a table from another tree meant an indirect jump into
        // whatever happened to be there, so it had to be recognised first.
        //
        // What replaces it is a better check, and it happens in bind_imports():
        // a vDSO that does not export what the program imports fails by name
        // ("the program needs 'ours_command_list', which the vDSO does not
        // export"), before anything runs. A version number says two images
        // disagree; this says which call is missing.
        log::info("userboot: vdso mapped [{:X}, {:X})", out.base, out.base + out.span);
        return Status::Ok;
    }
} // namespace

auto launch(ustl::Arc<object::JobDispatcher> job, std::string_view const name,
            ustl::Box<object::Handle> bootstrap) -> Status {
    if (!resident_package) {
        log::error("userabi: launch('{}') before the kernel package was loaded", name);
        return Status::BadState;
    }
    auto const &package = *resident_package;

    // A C string for the process and the mappings, and a hard limit: a name
    // cut short would be looked up, not found, and reported as missing.
    ProgramName program{};
    if (name.size() >= sizeof(program.text)) {
        return Status::OutOfRange;
    }
    std::memcpy(program.text, name.data(), name.size());

    // The vDSO is in the package beside the programs, but it is not one:
    // it is mapped into every process by this function, and a process
    // started *from* it would begin at an e_entry that means nothing.
    // check_program() below would refuse it as well, for its entry lying in
    // a non-executable segment; it is named here so the refusal says why.
    if (program.view() == VDSO_FILE) {
        log::error("userabi: '{}' is the vDSO, not a program", program.view());
        return Status::InvArgs;
    }

    auto const image = package.file(program.view());
    if (image.is_err()) {
        log::error("userabi: '{}' is not in the kernel package: {}", program.view(),
                   to_string(image.unwrap_err()));
        return Status::NotFound;
    }

    // Checked before the process exists, not left to the loader: until the
    // fault path can kill a process (design P1-A), a bad entry point is a
    // ring-3 fault the kernel cannot contain, and even after that a process
    // that dies on its first instruction is a worse answer to the caller
    // than an error from this call.
    if (auto const status = elfload::check_program(image.unwrap()); Status::Ok != status) {
        log::error("userabi: '{}' is not a program this kernel can start", program.view());
        return status;
    }

    log::info("userboot: found '{}', {} bytes of ELF", program.view(), image.unwrap().size());

    // One call builds both the process and the only object that may map into
    // it. They come back together because neither is useful alone: a process
    // with no root VMA cannot be given any memory, and a VMA dispatcher with
    // no process names a region nothing will ever run in.
    object::KernelHandle<object::ProcessDispatcher> process;
    object::KernelHandle<object::VmaDispatcher> root_vma;
    Rights process_rights;
    Rights root_vma_rights;
    auto status = object::ProcessDispatcher::create(job, program.text, process, process_rights,
                                                    root_vma, root_vma_rights);
    if (Status::Ok != status) {
        log::error("userabi: could not create the process: {}", to_string(status));
        return status;
    }

    // One loader for this process's address space: every image it is built
    // out of goes through the same one, and the object that may map into it is
    // what they have in common.
    elfload::Loader const loader{*root_vma.dispatcher()};

    LoadedImage loaded;
    status = load_program(image.unwrap(), program.text, loader, *root_vma.dispatcher(), loaded);
    if (Status::Ok != status) {
        return status;
    }

    log::info("userboot: mapped [{:X}, {:X}) into its own aspace, entry at {:X}, stack {:X}",
              loaded.image.base, loaded.image.base + loaded.image.span, loaded.image.entry,
              loaded.stack_top);

    // The vDSO, mapped into the same address space. It is what the program
    // enters the kernel through, and the only thing that can: the syscall
    // path refuses a trap whose return address lies outside the range
    // recorded below.
    auto const vdso_bytes = package.file(VDSO_FILE);
    if (vdso_bytes.is_err()) {
        log::error("userboot: '{}' is not in the kernel package: {}", VDSO_FILE,
                   to_string(vdso_bytes.unwrap_err()));
        return Status::NotFound;
    }

    elfload::Image vdso;
    status = load_vdso(vdso_bytes.unwrap(), loader, vdso);
    if (Status::Ok != status) {
        return status;
    }

    // Both images are placed, so the program's imports can be resolved against
    // the vDSO's exports. This has to happen before the thread starts and
    // after both load addresses are known, which is the only reason the
    // program is not simply bound as it is mapped.
    auto const binder = elfload::Binder::from(vdso_bytes.unwrap(), vdso);
    if (binder.is_err()) {
        log::error("userboot: the vDSO cannot be bound against: {}",
                   to_string(binder.unwrap_err()));
        return binder.unwrap_err();
    }
    status = binder.unwrap().bind(image.unwrap(), loaded.image);
    if (Status::Ok != status) {
        return status;
    }

    // Recorded on the process, not in a global: the range is a property of one
    // address space, and the syscall path reaches it through the thread that
    // trapped.
    process.dispatcher()->set_vdso_code_range(vdso.base, vdso.span);

    // The thread comes from the object layer too: ThreadDispatcher::spawn()
    // does the aspace wiring (it knows the process), and its trampoline builds
    // the frame that enters ring 3.
    object::KernelHandle<object::ThreadDispatcher> thread;
    status = object::ThreadDispatcher::spawn(process.dispatcher(), program.text, &thread);
    if (Status::Ok != status) {
        log::error("userabi: could not create the entry thread: {}", to_string(status));
        return status;
    }

    // The one handle the program starts with goes into its own table, and its
    // value into %rdi. A process may also start with none (%rdi = 0).
    auto bootstrap_value = HANDLE_INVALID;
    if (bootstrap) {
        bootstrap_value = process.dispatcher()->handle_table().add(ustl::move(bootstrap));
    }
    log::info("userabi: '{}' starts with handle {:#x}", program.view(), bootstrap_value);

    // start(), not resume(): where a kernel thread only needs to be made
    // runnable, a user thread has to be told where in user space to begin.
    //
    // %rsi is the vDSO's base. It is not needed to make a system call -- the
    // program's imports are bound before it runs -- but it is the one fact
    // about its own address space a program cannot otherwise find, and a
    // runtime that walks its own stack or binds itself later (P2) wants it.
    status = thread.dispatcher()->start({
        .entry = loaded.image.entry,
        .stack = initial_stack_pointer(loaded.stack_top),
        .arg1 = bootstrap_value,
        .arg2 = vdso.base,
    });
    if (Status::Ok != status) {
        log::error("userabi: could not start '{}': {}", program.view(), to_string(status));
        return status;
    }

    // Every handle in this function goes out of scope here, and nothing leaks:
    // the kernel thread holds the ThreadDispatcher, which holds the
    // ProcessDispatcher, which owns the address space the mappings live in.
    // The VMA and VMO dispatchers were the objects that were *allowed* to
    // create those mappings, not the things that hold them.
    return Status::Ok;
}

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

    // Kept, so that programs started later come out of the same package.
    resident_package = ustl::some(package.unwrap());

    // Whichever program the image asked for (`kernel.select.userboot`, see
    // boot::BootOptions::userboot()). A name that is not in the package is a
    // hard error rather than something to fall back from: the image said
    // what it wants started, and quietly starting a different program would
    // turn a packaging mistake into a boot that looks fine and is not the one
    // that was configured.
    auto const program = boot::options().userboot();

    // The first process is handed the root job, as a handle in its own table.
    // A job is what processes are created under, so the program that holds
    // the root one is the one that can start the rest of the system -- what
    // Zircon's userboot receives in its bootstrap message. Default rights;
    // narrowing them is a decision for when there is more than one program.
    auto bootstrap =
        object::Handle::make(ustl::Arc<object::Dispatcher>(object::JobDispatcher::root_job()),
                             object::JobDispatcher::default_right());
    if (!bootstrap) {
        log::error("userabi: could not make a handle to the root job");
        return;
    }
    log::info("userabi: handing {} the root job", program);

    auto const status = launch(object::JobDispatcher::root_job(), program, ustl::move(bootstrap));
    if (Status::Ok != status) {
        log::error("userboot: {}={} could not be started: {}", boot::BootOptions::USERBOOT_OPTION,
                   program, to_string(status));
    }
}
} // namespace userabi
