/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// The kernel side of userboot: find the first user program, build a process
/// around it, hand it what it starts with, and enter it.
#include <userabi/userboot.hpp>

#include <algorithm>
#include <string_view>

#include <cstring>

#include "resource.hpp"
#include "user_bootfs.hpp"
#include <elfload/binder.hpp>
#include <elfload/program.hpp>
#include <heap/new.hpp>
#include <kelfload/loader.hpp>
#include <logz4/log.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/assert.hpp>
#include <ours/boot/handoff.hpp>
#include <ours/boot/options.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/channel_dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/handle_table.hpp>
#include <ours/object/job_dispatcher.hpp>
#include <ours/object/message_packet.hpp>
#include <ours/object/process_dispatcher.hpp>
#include <ours/object/thread_dispatcher.hpp>
#include <ours/object/vma_dispatcher.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ustl/mem/align.hpp>

namespace userabi {
using namespace ours;

namespace {
    /// The vDSO's name in the same BootFS, installed by
    /// //kernel/lib/userabi/vdso with the same has_patches = false convention.
    constexpr std::string_view VDSO_FILE{"vdso"};

    /// elfload's reports as lines in the kernel log: the lines elfload used to
    /// write itself, before it took a Diagnostics so that a user program could
    /// link it without logz4. The Note is the one it wrote at info: where an
    /// import was bound, the first thing a later fault through it needs.
    auto log_elfload(void *, elfload::Report const &report) -> void {
        u64 value = 0;
        if (report.value.is_some()) {
            value = report.value.value();
        }
        if (report.severity == elfload::Severity::Note) {
            log::info("elfload: {} '{}' to {:X}", report.what, report.name, value);
            return;
        }
        if (!report.name.empty() && report.value.is_some()) {
            log::error("elfload: {}: {} at {:#x}", report.what, report.name, value);
        } else if (!report.name.empty()) {
            log::error("elfload: {}: {}", report.what, report.name);
        } else if (report.value.is_some()) {
            log::error("elfload: {}: {:#x}", report.what, value);
        } else {
            log::error("elfload: {}", report.what);
        }
    }

    constexpr elfload::Diagnostics ELFLOAD_LOG{.report = log_elfload, .context = nullptr};

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

    /// The vDSO, laid out in one VMO once and mapped into every process
    /// launch() starts. Zircon's VDso::Create() makes its instance_ the same
    /// way (zircon/kernel/lib/userabi/vdso.cc), and every process maps that one
    /// VMO.
    ///
    /// It used to be laid out again for each process -- launch() called
    /// Loader::map() on the vDSO's file, which copies it into a fresh VMO --
    /// so every process carried a private copy of the vDSO's pages. Written
    /// once by lay_out_vdso(), before any user thread exists, and only read
    /// after that.
    struct SharedVdso {
        kelfload::ImageVmo image;

        /// The vDSO's file in the resident package, which the Binder reads the
        /// vDSO's exports from when it binds a program.
        ustl::views::Span<u8 const> file;

        /// Laid out yet: the VMO handle is empty until lay_out_vdso() runs.
        /// Not a ustl::Option, whose move has to be noexcept, which
        /// KernelHandle's is not.
        [[nodiscard]]
        auto ready() const -> bool {
            return static_cast<bool>(image.vmo.dispatcher());
        }
    };
    SharedVdso shared_vdso;

    /// The BootFS user space is given (make_user_bootfs()), built once from
    /// the resident package, and the rights a handle to it is to carry.
    ///
    /// Held here only between being built and being sent: write_bootstrap()
    /// moves it into the first process's processargs message, and this is
    /// empty from then on. The first process decides who else gets it.
    object::KernelHandle<object::VmoDispatcher> resident_bootfs;
    Rights resident_bootfs_rights;

    /// The command-line options that belong to user space rather than to the
    /// kernel: `userboot.*`, the userboot program's own -- `userboot.next`,
    /// the program it starts next, first among them -- and `ours.autorun.*`,
    /// which init reads (`ours.autorun.boot`, a program to start once the
    /// shell is up). The kernel parses neither family; it only decides that
    /// they cross.
    ///
    /// The table is an allow list for what of the image's command line
    /// crosses into user space: the kernel's own options (`kernel.shell`,
    /// `kernel.name`, ...) steer ring 0 and stay there, and a new kernel
    /// option does not leak out by default because nobody remembered to
    /// filter it. A new user-space option family is one more row here.
    ///
    /// Zircon draws the userboot half of this line by name -- its userboot
    /// parses only `userboot.*` keys (parse_option() in
    /// zircon/kernel/lib/userabi/userboot/main.rs) -- but after the fact,
    /// because it reads the whole CMDLINE item out of the ZBI VMO it is
    /// handed, and hands that ZBI on so that component_manager can serve the
    /// rest (`zircon.autorun.boot` among it) as fuchsia.boot.Arguments. Here
    /// the kernel forwards the tokens instead, as Zircon's userboot once
    /// received the command line as its environment, and the userboot program
    /// passes on what is not its own; nobody in user space needs a
    /// boot-container parser.
    constexpr std::string_view USER_OPTION_PREFIXES[] = {
        "userboot.",
        "ours.autorun.",
    };

    /// The user-space tokens of the image's command line, in order.
    ///
    /// At most MAX_USER_OPTIONS of them, USER_OPTIONS_BYTES in all
    /// (<ours/process_args.hpp>): the message is built in a fixed buffer on
    /// this thread's stack (write_bootstrap()), and the programs that pass
    /// the options on size theirs from the same bound.
    ///
    /// Views into the boot package's Config item, which stays mapped for the
    /// kernel's lifetime; they are only held until write_bootstrap() has
    /// copied them into the message.
    struct UserOptions {
        std::string_view tokens[MAX_USER_OPTIONS];
        u32 count{0};
    };

    /// Whether |token| belongs to one of the USER_OPTION_PREFIXES families.
    auto is_user_option(std::string_view const token) -> bool {
        for (auto const prefix: USER_OPTION_PREFIXES) {
            if (token.starts_with(prefix)) {
                return true;
            }
        }
        return false;
    }

    /// Picks the user-space tokens out of the image's command line.
    ///
    /// Not a boot::BootOptions entry, although that is where the kernel's
    /// options are parsed: nothing here is parsed. The kernel neither knows
    /// which user-space keys exist nor what their values mean -- that is the
    /// user programs' to decide -- so it relays whole tokens, and
    /// BootOptions' rule of one name, one default and one grammar per option
    /// has nothing to hold on to.
    ///
    /// A token that does not fit is dropped and named, not truncated, and the
    /// rest are still forwarded: a truncated `userboot.next=` would start a
    /// program nobody asked for, while a dropped one leaves userboot on its
    /// defaults with the reason in the log -- as boot::BootOptions ignores a
    /// value it cannot take rather than refusing to boot.
    auto collect_user_options() -> UserOptions {
        UserOptions options;
        auto const package = boot::boot_package();
        if (package.is_err()) {
            return options;
        }
        auto const cmdline = package.unwrap().command_line();
        if (cmdline.is_none()) {
            return options;
        }

        usize bytes = 0;
        cmdline.value().for_each_token([&options, &bytes](std::string_view const token) -> void {
            if (!is_user_option(token)) {
                return;
            }
            if (options.count == MAX_USER_OPTIONS ||
                bytes + token.size() + 1 > USER_OPTIONS_BYTES) {
                log::error("userabi: dropping {}: no room for it in the first process's "
                           "processargs",
                           token);
                return;
            }
            options.tokens[options.count++] = token;
            bytes += token.size() + 1;
        });
        return options;
    }

    /// The first process's bootstrap channel. The end returned is the one the
    /// process gets in %rdi; |kernel_end| stays here, to write the processargs
    /// on once the process exists (write_bootstrap()), because one of the
    /// handles in it -- the root VMA -- only exists from then on.
    auto make_bootstrap_channel(object::KernelHandle<object::ChannelDispatcher> &kernel_end)
        -> ustl::Box<object::Handle> {
        object::KernelHandle<object::ChannelDispatcher> user_end;
        Rights channel_rights;
        if (auto const status =
                object::ChannelDispatcher::create(&kernel_end, &user_end, &channel_rights);
            Status::Ok != status) {
            log::error("userabi: could not make the bootstrap channel: {}", to_string(status));
            return {};
        }
        return object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(user_end)),
                                    channel_rights);
    }

    /// The first process's processargs message (design 3.3), written on
    /// |kernel_end| after the process was launched: the order a launcher
    /// keeps, and Zircon's kernel keeps for its userboot.
    ///
    /// The message carries argv = {|program|}, the environment
    /// collect_user_options() finds -- the image's user-space options, which
    /// is how the userboot program learns `userboot.next` and init, through
    /// it, `ours.autorun.boot` -- and the startup handles:
    ///
    ///   JobDefault  the root job, with its default rights -- what processes
    ///               are created under, so the first process is the one that
    ///               can start the rest of the system;
    ///   VmoBootfs   the user BootFS (resident_bootfs), when there is one:
    ///               moved into the message, not copied, as Zircon's userboot
    ///               moves its VMO handles out;
    ///   VmaRoot     |root_vma|, the process's own root region, which launch()
    ///               handed back;
    ///   VmoVdso     the vDSO's VMO, to map it into the processes this one is
    ///               to start.
    ///
    /// Everything is made on this kernel thread, which belongs to no process,
    /// so none of these handles is charged to anyone's quota (Handle::make()
    /// charges the current process, and there is none).
    ///
    /// The caller drops the kernel's end after this, as a KernelHandle, whose
    /// destructor tells the channel its last holder is gone. Its message
    /// stays queued and readable; the process sees PEER_CLOSED alongside it,
    /// which the runtime is ready for (it waits for READABLE or PEER_CLOSED,
    /// then reads). Zircon drops its end too, but takes it out of its
    /// KernelHandle with release() first (BootstrapChannel::Create), which
    /// skips that notice: its userboot never sees PEER_CLOSED on the channel.
    ///
    /// Says why on any failure.
    auto write_bootstrap(object::ChannelDispatcher &kernel_end, std::string_view const program,
                         ustl::Box<object::Handle> root_vma) -> Status {
        // Only handles that exist go in, and each info beside its handle in
        // the same position: MessagePacket takes the kernel's word that none
        // is empty.
        ustl::Box<object::Handle> handles[5];
        u32 infos[5] = {};
        u32 count = 0;
        handles[count] = ustl::move(root_vma);
        infos[count++] = handle_info(HandleType::VmaRoot, 0);
        handles[count] =
            object::Handle::make(ustl::Arc<object::Dispatcher>(object::JobDispatcher::root_job()),
                                 object::JobDispatcher::default_right());
        if (!handles[count]) {
            log::error("userabi: could not make a handle to the root job");
            return Status::NoResource;
        }
        infos[count++] = handle_info(HandleType::JobDefault, 0);

        if (resident_bootfs.dispatcher()) {
            handles[count] = object::Handle::make(
                object::KernelHandle<object::Dispatcher>(ustl::move(resident_bootfs)),
                resident_bootfs_rights);
            if (!handles[count]) {
                log::error("userabi: could not make a handle to the BootFS");
                return Status::NoResource;
            }
            infos[count++] = handle_info(HandleType::VmoBootfs, 0);
        }

        // vmex: the one capability that turns a VMO executable (design D3,
        // K18). Minted here and nowhere else, so the first process is the only
        // one that ever holds it, and it decides who gets it next. The
        // handle_info's argument is the slot, as Zircon's userboot tags its
        // system resources: the receiver asks for SystemResource with the
        // slot it wants.
        handles[count] = make_system_resource_handle(RSRC_SYSTEM_VMEX_BASE, "vmex");
        if (!handles[count]) {
            return Status::NoResource;
        }
        infos[count++] = handle_info(HandleType::SystemResource, u16(RSRC_SYSTEM_VMEX_BASE));

        // The vDSO's VMO, for the launcher the first process is to become
        // (K23): it maps the vDSO into each process it starts, and the kernel
        // takes the mapping as the registration of that process's vDSO range.
        // Read, map and execute, never write, and no other object has these
        // rights over the vDSO's pages; the rights are the VMO defaults less
        // Write plus Execute, which is a capability here as for any VMO
        // (design D3) and is given because the kernel made this VMO itself.
        handles[count] =
            object::Handle::make(ustl::Arc<object::Dispatcher>(shared_vdso.image.vmo.dispatcher()),
                                 (object::RIGHTS_DEFAULT_VMO & ~Rights::Write) | Rights::Execute);
        if (!handles[count]) {
            log::error("userabi: could not make a handle to the vDSO's VMO");
            return Status::NoResource;
        }
        infos[count++] = handle_info(HandleType::VmoVdso, 0);

        // Sized to hold the most this can ever write, so that encoding cannot
        // fail for want of room: the header, five handle_infos, a program name
        // of at most 63 bytes and its NUL (boot::BootOptions keeps it in 64),
        // and the environment collect_user_options() has already bounded.
        constexpr usize MESSAGE_BYTES =
            sizeof(ProcArgs) + 5 * sizeof(u32) + 64 + USER_OPTIONS_BYTES;
        u8 message[MESSAGE_BYTES];
        u32 length = 0;
        std::string_view const args[] = {program};
        auto const environ = collect_user_options();
        for (auto index = u32(0); index < environ.count; ++index) {
            log::info("userabi: passing {} to {}", environ.tokens[index], program);
        }
        if (auto const status = encode_process_args(message, {infos, count}, args,
                                                    {environ.tokens, environ.count}, {}, &length);
            Status::Ok != status) {
            log::error("userabi: could not encode the processargs: {}", to_string(status));
            return status;
        }

        ustl::Box<object::MessagePacket> packet;
        if (auto const status =
                object::MessagePacket::create({message, length}, handles, count, &packet);
            Status::Ok != status) {
            log::error("userabi: could not make the processargs packet: {}", to_string(status));
            return status;
        }
        if (auto const status = kernel_end.write(ustl::move(packet)); Status::Ok != status) {
            log::error("userabi: could not queue the processargs: {}", to_string(status));
            return status;
        }
        return Status::Ok;
    }

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
                      kelfload::Loader const &loader, object::VmaDispatcher &root_vma,
                      LoadedImage &out) -> Status {
        // Mapped through the root VMA, which always maps for ring 3 (User).
        // Writable as well as executable for now because the three
        // segments (R, RX, RW) are being mapped as one range; giving each its
        // own permissions is a later step, and one worth taking -- an image
        // that is writable and executable at once is exactly what W^X exists
        // to forbid.
        //
        // Note it does not defeat the vDSO rule: a program that writes a
        // `syscall` into its own text still traps from an address outside the
        // vDSO, which is exactly what the kernel refuses.
        constexpr auto IMAGE_PERMS =
            VmOptions::PermRead | VmOptions::PermWrite | VmOptions::PermExecute;

        auto status = loader.map(bytes, USER_ASPACE_BASE, IMAGE_PERMS, name, out.image);
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

        //
        // At its fixed place, populated now, and never executable later
        // either: the ceiling is read and write.
        constexpr auto STACK_OPTIONS = VmOptions::PermRead | VmOptions::PermWrite |
                                       VmOptions::CanMapRead | VmOptions::CanMapWrite |
                                       VmOptions::Specific | VmOptions::MapRange;
        auto stack =
            root_vma.map(STACK_OPTIONS, USER_STACK_TOP - USER_STACK_SIZE - root_vma.vma()->base(),
                         *stack_vmo.dispatcher(), 0, USER_STACK_SIZE, "userboot-stack");
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

    /// Maps the shared vDSO, which is the image every user program's imports
    /// are bound to, into the address space |loader| is for.
    ///
    /// Read-only and executable, never writable: this is the only code in the
    /// address space allowed to enter the kernel, so a program that could
    /// rewrite it could place a trap anywhere inside the one range the kernel
    /// trusts.
    auto map_vdso(kelfload::Loader const &loader, elfload::Image &out) -> Status {
        // Sealed, which the kernel requires of the vDSO's VMO: nothing may
        // unmap it or change its protection once it is the range system calls
        // are accepted from.
        constexpr auto VDSO_PERMS =
            VmOptions::PermRead | VmOptions::PermExecute | VmOptions::Sealed;

        auto const status = loader.map(shared_vdso.image, VDSO_BASE, VDSO_PERMS, "vdso", out);
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

    /// Lays the vDSO out once, for launch() to map into every process.
    auto lay_out_vdso(phys::KernelPackage const &package) -> Status {
        auto const file = package.file(VDSO_FILE);
        if (file.is_err()) {
            log::error("userabi: '{}' is not in the kernel package: {}", VDSO_FILE,
                       to_string(file.unwrap_err()));
            return file.unwrap_err();
        }

        kelfload::ImageVmo image;
        if (auto const status = kelfload::Loader::load(file.unwrap(), "vdso", image);
            Status::Ok != status) {
            return status;
        }
        auto const pages = image.span / PAGE_SIZE;
        // Marked before anything can map it: from here a mapping of this VMO
        // registers an address space's vDSO range (VmaDispatcher::map()).
        image.vmo.dispatcher()->mark_vdso();
        shared_vdso.image = ustl::move(image);
        shared_vdso.file = file.unwrap();
        log::info("userabi: vdso laid out once, {} page(s), shared by every process", pages);
        return Status::Ok;
    }
} // namespace

auto launch(ustl::Arc<object::JobDispatcher> job, std::string_view const name,
            ustl::Box<object::Handle> bootstrap, ustl::Box<object::Handle> *const root_vma_out)
    -> Status {
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
    if (auto const status = elfload::check_program(ELFLOAD_LOG, image.unwrap());
        Status::Ok != status) {
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
    kelfload::Loader const loader{*root_vma.dispatcher()};

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
    if (!shared_vdso.ready()) {
        log::error("userabi: launch('{}') before the vDSO was laid out", name);
        return Status::BadState;
    }

    elfload::Image vdso;
    status = map_vdso(loader, vdso);
    if (Status::Ok != status) {
        return status;
    }

    // Both images are placed, so the program's imports can be resolved against
    // the vDSO's exports. This has to happen before the thread starts and
    // after both load addresses are known, which is the only reason the
    // program is not simply bound as it is mapped.
    auto const binder = elfload::Binder::from(ELFLOAD_LOG, shared_vdso.file, vdso);
    if (binder.is_err()) {
        log::error("userboot: the vDSO cannot be bound against: {}",
                   to_string(binder.unwrap_err()));
        return binder.unwrap_err();
    }
    status = binder.unwrap().bind(ELFLOAD_LOG, image.unwrap(), loaded.image);
    if (Status::Ok != status) {
        return status;
    }

    // Nothing to record: mapping the vDSO's VMO is what registered its range,
    // on the address space, and the syscall path reaches it through the thread
    // that trapped (VmaDispatcher::map()).

    // The thread comes from the object layer too: ThreadDispatcher::create()
    // makes the dispatcher, start() below makes its kernel thread with the
    // aspace wiring (it knows the process), and the trampoline builds the
    // frame that enters ring 3.
    object::KernelHandle<object::ThreadDispatcher> thread;
    status = object::ThreadDispatcher::create(process.dispatcher(), program.text, &thread);
    if (Status::Ok != status) {
        log::error("userabi: could not create the entry thread: {}", to_string(status));
        return status;
    }

    // The root VMA goes back to whoever asked for the launch, who puts it in
    // the processargs it writes once this returns: the one message a process
    // gets is written after the process exists, as a launcher does it --
    // first process_create, which hands back the root VMA, then the message
    // (Zircon's process_builder; design 3.2). Made before the thread starts,
    // so that a handle that cannot be made fails the launch rather than
    // leaving a process running that nobody can give its region to.
    // TEMPORARY in this form: until K23 the launch itself is in the kernel.
    if (root_vma_out) {
        *root_vma_out = object::Handle::make(
            object::KernelHandle<object::Dispatcher>(ustl::move(root_vma)), root_vma_rights);
        if (!*root_vma_out) {
            return Status::NoResource;
        }
    }

    log::info("userabi: '{}' starts with {}", program.view(),
              bootstrap ? "a bootstrap handle" : "no handle");

    // The same call a user-space launcher makes (process_start): the one
    // handle the program starts with goes into its own table and its value
    // into %rdi -- a process may also start with none (%rdi = 0) -- and a
    // thread is told where in user space to begin.
    //
    // %rsi is the vDSO's base. It is not needed to make a system call -- the
    // program's imports are bound before it runs -- but it is the one fact
    // about its own address space a program cannot otherwise find, and a
    // runtime that walks its own stack or binds itself later (P2) wants it.
    status = process.dispatcher()->start(*thread.dispatcher(), loaded.image.entry,
                                         initial_stack_pointer(loaded.stack_top),
                                         ustl::move(bootstrap), vdso.base);
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

    // The vDSO, once, before the first process: no process can run without
    // it, so failing here is failing to start user space at all.
    if (auto const status = lay_out_vdso(*resident_package); Status::Ok != status) {
        log::error("userabi: could not lay out the vDSO: {}", to_string(status));
        return;
    }

    // Built before the first process starts, so that the VMO exists by the
    // time anything could ask for it. A failure is logged inside and is not
    // fatal: the first program runs without a BootFS, which it can say, and
    // which is less than it would otherwise get rather than more.
    //
    // The decompressed package stays where it is. Every program is still
    // loaded out of it (launch()), and the kernel's own files are in it.
    static_cast<void>(make_user_bootfs(*resident_package, boot::options().kernel_name(),
                                       resident_bootfs, resident_bootfs_rights));

    // The userboot program, always: `kernel.select.userboot` only picks which
    // build of it (see boot::BootOptions::userboot()), and what runs after it
    // is its own decision, from the `userboot.next` that write_bootstrap()
    // forwards. A name that is not in the package is a hard error rather
    // than something to fall back from: the image said which userboot it
    // wants, and quietly starting a different program would turn a packaging
    // mistake into a boot that looks fine and is not the one that was
    // configured.
    auto const program = boot::options().userboot();

    // The first process starts like every other one on the runtime: a
    // bootstrap channel in %rdi with a processargs message on it. The process
    // first, then the message (write_bootstrap()): its root VMA is one of the
    // handles in it, and that only exists once launch() has made the process.
    // The runtime waits for the message, so a thread that starts before it
    // lands just waits. What it gets is its root VMA, the root job, the
    // BootFS and the user-space options -- everything userboot needs to
    // start the next program and hand the rest on to it.
    auto const has_bootfs = static_cast<bool>(resident_bootfs.dispatcher());
    object::KernelHandle<object::ChannelDispatcher> kernel_end;
    auto bootstrap = make_bootstrap_channel(kernel_end);
    if (!bootstrap) {
        return;
    }

    ustl::Box<object::Handle> root_vma;
    auto status =
        launch(object::JobDispatcher::root_job(), program, ustl::move(bootstrap), &root_vma);
    if (Status::Ok != status) {
        log::error("userboot: {}={} could not be started: {}", boot::BootOptions::USERBOOT_OPTION,
                   program, to_string(status));
        return;
    }

    log::info("userabi: handing {} its processargs: its root VMA, the root job{}", program,
              has_bootfs ? " and the BootFS" : "");
    // A failure is said inside. The process is already running, and with the
    // kernel's end dropped below it sees PEER_CLOSED and no message, and ends
    // itself (runtime::EXIT_START_FAILED).
    status = write_bootstrap(*kernel_end.dispatcher(), program, ustl::move(root_vma));
    static_cast<void>(status);
}
} // namespace userabi
