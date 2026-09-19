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

#include <eftl/elf/dynamic_image.hpp>
#include <eftl/elf/mapped_memory.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/symbol.hpp>
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
    /// The default first user program, and the name its ELF has inside the
    /// kernel package's BootFS.
    ///
    /// Not "userboot/image.elf". `kernel_elf_binary("userboot")` in
    /// //kernel/lib/userabi/userboot/BUILD.gn sets `has_patches = false`,
    /// and kernel_elf_image() installs a patch-less binary as one bare file
    /// rather than as a `<name>/` subtree -- which is why vmours, which does
    /// have patches, is "vmours/image.elf" instead. The two live in the same
    /// BootFS with different shapes, so this cannot be derived from the
    /// kernel's own name; it has to be stated.
    constexpr std::string_view USERBOOT_FILE{"userboot"};

    /// The boot option that names the program the one user process runs:
    /// `kernel.select.userboot=<name>`, a file in the kernel package.
    ///
    /// This is Zircon's option of the same name (its default is likewise
    /// "userboot"; see lib/boot-options/options.inc), taken over wholesale
    /// because the property it encodes is the same one: *which* program runs
    /// first belongs to the image being booted, not to the kernel. What stood
    /// here before was a compiled-in preference -- start "shell", fall back to
    /// "userboot" when the package has no shell -- which put the name of an
    /// //examples target inside the kernel and made "what ran" a function of
    /// what a package happened to contain rather than of anything the image
    /// states. An image that wants a different program now says so
    /// (//kernel/products/image/dev/BUILD.gn).
    ///
    /// Zircon resolves the option one stage earlier, in physboot:
    /// HandoffPrep::UsePackageFiles() matches it against the kernel package
    /// and hands the kernel a loaded ELF. ours::phys::Handoff deliberately
    /// carries neither the command line nor a userboot image -- its comment
    /// says vmours can re-derive the command line from boot_package(), so a
    /// second copy would be two answers to one question -- so the lookup
    /// happens here instead, against that same OBI container.
    constexpr std::string_view USERBOOT_OPTION{"kernel.select.userboot"};

    /// The vDSO's name in the same BootFS, installed by
    /// //kernel/lib/userabi/vdso with the same has_patches = false convention.
    constexpr std::string_view VDSO_FILE{"vdso"};

    /// A program name, kept as a NUL-terminated buffer rather than as a view.
    ///
    /// The option's value is a slice of the command-line item's payload that
    /// ends at a space or at the end of the line, while ProcessDispatcher and
    /// the loader below both want a C string; copying once here is cheaper
    /// than having each caller find that out. The capacity matches the
    /// fixed-size option strings phys::BootOptions already keeps for
    /// `kernel.phys.next` and `kernel.name`, which is the same 64 characters
    /// Zircon's SmallString holds this option in.
    struct ProgramName {
        char text[64]{};

        [[nodiscard]]
        auto view() const -> std::string_view {
            return {text};
        }
    };

    /// Resolves `kernel.select.userboot` against the OBI command line.
    ///
    /// Falls back to USERBOOT_FILE whenever the container has no command line
    /// or the option is absent, which is the common case: only an image that
    /// wants something other than the proof-of-life program writes it.
    ///
    /// The command line comes from the OBI container, never from the
    /// bootloader: GRUB's command line steers phys alone, and by the time
    /// vmours runs, the options that survive are the ones the image baked into
    /// its Config item.
    auto select_userboot() -> ProgramName {
        ProgramName name{};
        auto selected = USERBOOT_FILE;

        auto const package = boot::boot_package();
        if (package.is_ok()) {
            auto const cmdline = package.unwrap().command_line();
            if (cmdline.is_some()) {
                auto const value = cmdline.value().value(USERBOOT_OPTION);
                if (value.is_some()) {
                    // A name that does not fit is rejected rather than
                    // truncated: a truncated name would be looked up, not
                    // found, and reported as a missing file -- which sends the
                    // reader looking in the package for something the kernel
                    // itself mangled.
                    if (value.value().size() < sizeof(name.text)) {
                        selected = value.value();
                    } else {
                        log::error("userboot: {}={} is longer than {} characters, using '{}'",
                                   USERBOOT_OPTION, value.value(), sizeof(name.text) - 1,
                                   USERBOOT_FILE);
                    }
                }
            }
        }

        std::memcpy(name.text, selected.data(), selected.size());
        return name;
    }

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

    /// The x86-64 relocation types this loader applies.
    ///
    /// Four, and no more, because of how the images are built: a program is
    /// compiled -fno-plt and linked -z now, so an imported function reaches
    /// the GOT the way an imported variable does (GLOB_DAT), and there is no
    /// lazily-bound stub to service. JUMP_SLOT is accepted anyway because a
    /// linker may still emit one for an address taken rather than called, and
    /// with BIND_NOW it means the same thing: store the resolved address.
    /// RELATIVE is the image's own absolute pointers; R_X86_64_64 is a
    /// symbol's address stored whole.
    constexpr u32 R_X86_64_64 = 1;
    constexpr u32 R_X86_64_GLOB_DAT = 6;
    constexpr u32 R_X86_64_JUMP_SLOT = 7;
    constexpr u32 R_X86_64_RELATIVE = 8;

    /// One mapped ELF image: where it landed, how much of the address space it
    /// covers, what its e_entry resolved to, and how the kernel reaches its
    /// bytes.
    ///
    /// |view| and |image_base| exist for the binding pass below. Relocations
    /// have to be written into the image *before* it runs, and the kernel
    /// cannot write through the user mapping -- that mapping is in another
    /// address space, and writing to those addresses from here would hit
    /// whatever the current address space has there. |view| is the kernel's
    /// own pointer to the same frames, so a write through it lands in the page
    /// the program will read. |image_base| is the lowest p_vaddr the ELF
    /// claims, which is what every address in its dynamic tables is relative
    /// to; `base - image_base` is the load bias.
    struct MappedImage {
        VirtAddr base = 0;
        usize span = 0;
        VirtAddr entry = 0;
        u8 *view = nullptr;
        VirtAddr image_base = 0;
    };

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
        MappedImage image;
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

    /// Lays an ELF's PT_LOAD segments out in fresh frames and maps them into a
    /// user address space at |where|.
    ///
    /// Placement only: whatever the image needs fixing up afterwards is
    /// bind_imports()' business, and the two are kept apart because the order
    /// between them is forced -- an image cannot be bound to the vDSO before
    /// the vDSO has a load address, so every image is placed first and bound
    /// second.
    ///
    /// The segments land in one contiguous run of frames, from the lowest
    /// p_vaddr any PT_LOAD claims to the highest. That is a simplification
    /// (a real loader honours each segment's own permissions and the gaps
    /// between them), and it is also the invariant bind_imports() relies on
    /// when it describes the image to eftl as a single load region.
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
        out.view = backing;
        out.image_base = lowest;
        // e_entry is a file-relative address for a PIE, so it moves with the
        // load base like everything else. It is the first instruction of a
        // program; for the vDSO it means nothing at all -- nothing jumps to a
        // vDSO -- and is recorded here only because one routine maps both.
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

        auto status = map_elf_image(bytes, root_vma, mm::VmPlacement::at(USER_ASPACE_BASE),
                                    IMAGE_FLAGS, name, out.image);
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

    /// Resolves one symbol name against an image's dynamic symbol table.
    ///
    /// A linear scan, and it stays one until the table stops being two entries
    /// long: the vDSO publishes one symbol per system call, a program imports
    /// each of them once, and building either hash table eftl can read
    /// (DT_HASH, DT_GNU_HASH) would cost more than the scans it saves. The
    /// tables are there in the image -- this just does not need them yet.
    ///
    /// Returns the symbol's link-time value, which the caller turns into an
    /// address by adding the exporter's load bias.
    auto find_export(eftl::elf::SymbolTableView<eftl::elf::TraitsNative> const &symbols,
                     std::string_view const name) -> ustl::Option<u64> {
        for (usize index = 0; index < symbols.size(); ++index) {
            auto const spelling = symbols.symbol_name(index);
            if (spelling.is_none()) {
                continue;
            }
            auto const text = std::string_view(reinterpret_cast<char const *>(spelling->data()),
                                               spelling->size());
            if (text != name) {
                continue;
            }
            auto const symbol = symbols.symbol_at(index);
            // SHN_UNDEF in the exporter means the name is imported there too,
            // not defined -- binding to it would bind to nothing.
            if (symbol.is_none() || symbol->st_shndx == 0) {
                continue;
            }
            return ustl::some(static_cast<u64>(symbol->st_value));
        }
        return ustl::NONE;
    }

    /// Binds a loaded program's imports to the vDSO's definitions, and applies
    /// its own relative relocations.
    ///
    /// This is the whole of the dynamic linking a user program gets, and it is
    /// deliberately the smallest thing that deserves the name: one importer,
    /// one exporter, no DT_NEEDED search, no lookup scope, no lazy binding,
    /// no versioning. What it buys is that a program *calls* ours_debug_write()
    /// instead of carrying a hand-written copy of this function's job -- an
    /// export table, a magic and version check, and `table + offset` per entry,
    /// written once per program.
    ///
    /// It is the same pass ours::phys::ElfImage::relocate_and_bind() runs for a
    /// phys module against physload's export table, reduced to what one
    /// address space and one exporter need: the exporter here is an ELF rather
    /// than a generated table, so the definitions come out of .dynsym rather
    /// than out of resolve_export().
    ///
    /// A static image with no PT_DYNAMIC is not an error -- it has nothing to
    /// bind, which is exactly the shape userboot still has.
    auto bind_imports(ustl::views::Span<u8 const> program_file, MappedImage const &program,
                      ustl::views::Span<u8 const> vdso_file, MappedImage const &vdso) -> Status {
        using Traits = eftl::elf::TraitsNative;

        auto program_binary =
            eftl::elf::Parser::parse_as<Traits>(program_file.data(), program_file.size());
        if (program_binary.is_err()) {
            log::error("userboot: the program is not an ELF this loader can read");
            return Status::InvArgs;
        }

        auto const dynamic = program_binary->dynamic_image();
        if (dynamic.is_none()) {
            log::debug("userboot: the program has no PT_DYNAMIC; nothing to bind");
            return Status::Ok;
        }

        auto const relas = dynamic->dynamic_rela_table();
        auto const plt_relas = dynamic->dynamic_plt_rela_table();
        if (relas.is_none() && plt_relas.is_none()) {
            return Status::Ok;
        }

        auto const symbols = dynamic->dynamic_symbols();
        if (symbols.is_none()) {
            // Relocations that name symbols, with no table to name them from.
            log::error("userboot: the program has relocations but no readable .dynsym");
            return Status::InvArgs;
        }

        // The exporter's side of the contract.
        auto vdso_binary = eftl::elf::Parser::parse_as<Traits>(vdso_file.data(), vdso_file.size());
        if (vdso_binary.is_err()) {
            log::error("userboot: the vDSO is not an ELF this loader can read");
            return Status::InvArgs;
        }
        auto const vdso_dynamic = vdso_binary->dynamic_image();
        if (vdso_dynamic.is_none()) {
            log::error("userboot: the vDSO has no PT_DYNAMIC, so it exports nothing");
            return Status::InvArgs;
        }
        auto const exports = vdso_dynamic->dynamic_symbols();
        if (exports.is_none()) {
            log::error("userboot: the vDSO has no readable .dynsym");
            return Status::InvArgs;
        }

        // One region, because map_elf_image() laid the image out as one: every
        // link-time address from image_base to image_base + span appears at the
        // same offset in |view|. A loader that gave each segment its own
        // mapping would hand eftl one region per segment instead; the
        // translation it does is the same either way.
        eftl::elf::LoadRegion const region{
            .virtual_start = program.image_base,
            .virtual_end = program.image_base + program.span,
            .image_offset = 0,
            .permissions = eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable,
        };
        eftl::elf::MappedMemory<Traits> memory(program.view, program.span, {&region, 1});

        auto const program_bias = static_cast<u64>(program.base - program.image_base);
        auto const vdso_bias = static_cast<u64>(vdso.base - vdso.image_base);

        auto bind = [&](auto const &table) -> Status {
            for (usize index = 0; index < table.size(); ++index) {
                auto const entry = table.relocation_at(index);
                if (entry.is_none()) {
                    return Status::InvArgs;
                }
                auto const &rela = entry.value();
                auto const type = std::remove_cvref_t<decltype(table)>::type(rela);
                auto const symbol_index = std::remove_cvref_t<decltype(table)>::symbol_index(rela);

                u64 value = 0;
                if (type == R_X86_64_RELATIVE) {
                    // The image's own absolute pointers: no symbol involved,
                    // just "wherever this image landed".
                    value = program_bias + static_cast<u64>(rela.r_addend);
                } else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
                           type == R_X86_64_JUMP_SLOT) {
                    auto const symbol = symbols->symbol_at(static_cast<usize>(symbol_index));
                    auto const name = symbols->symbol_name(static_cast<usize>(symbol_index));
                    if (symbol.is_none() || name.is_none()) {
                        return Status::InvArgs;
                    }
                    auto const spelling = std::string_view(
                        reinterpret_cast<char const *>(name->data()), name->size());

                    if (symbol->st_shndx != 0) {
                        // Defined in the program itself; -Bsymbolic should have
                        // resolved it at link time, so reaching here means the
                        // linker left one for the loader.
                        value = program_bias + static_cast<u64>(symbol->st_value) +
                                static_cast<u64>(rela.r_addend);
                    } else {
                        auto const definition = find_export(exports.value(), spelling);
                        if (definition.is_none()) {
                            // Loud, and fatal. The alternative is binding to
                            // zero and faulting in ring 3 at a call whose
                            // target says nothing about which import was
                            // missing.
                            log::error("userboot: the program needs '{}', which the vDSO does "
                                       "not export",
                                       spelling);
                            return Status::NotFound;
                        }
                        value = vdso_bias + definition.value() + static_cast<u64>(rela.r_addend);
                        log::debug("userboot: bound '{}' to {:X}", spelling, value);
                    }
                } else {
                    log::error("userboot: the program carries relocation type {}, which this "
                               "loader does not apply",
                               type);
                    return Status::Unsupported;
                }

                // r_offset is a link-time address; MappedMemory translates it
                // through the region above into an offset in |view|. Adding the
                // bias here would double-count the load address.
                if (!memory.write<u64>(static_cast<u64>(rela.r_offset), value)) {
                    log::error("userboot: relocation {} writes outside the image", index);
                    return Status::OutOfRange;
                }
            }
            return Status::Ok;
        };

        if (relas.is_some()) {
            if (auto const status = bind(relas.value()); status != Status::Ok) {
                return status;
            }
        }
        if (plt_relas.is_some()) {
            if (auto const status = bind(plt_relas.value()); status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    /// Maps the vDSO, which is the image every user program's imports are
    /// bound to.
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

    // Whichever program the image asked for. A name that is not in the package
    // is a hard error rather than something to fall back from: the image said
    // what it wants started, and quietly starting a different program would
    // turn a packaging mistake into a boot that looks fine and is not the one
    // that was configured.
    auto const program = select_userboot();
    auto const image = package.unwrap().file(program.view());
    if (image.is_err()) {
        log::error("userboot: {}={} but no such file in the kernel package: {}", USERBOOT_OPTION,
                   program.view(), to_string(image.unwrap_err()));
        return;
    }

    log::info("userboot: found '{}', {} bytes of ELF", program.view(), image.unwrap().size());

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
        object::ProcessDispatcher::create(object::JobDispatcher::root_job(), program.text, process,
                                          process_rights, root_vma, root_vma_rights);
    if (Status::Ok != status) {
        log::error("userboot: could not create the process: {}", to_string(status));
        return;
    }

    LoadedImage loaded;
    // ProgramName::text is the C string these two want; that is what the type
    // exists for (see select_userboot()).
    if (Status::Ok != load_program(image.unwrap(), program.text, *root_vma.dispatcher(), loaded)) {
        return;
    }

    log::info("userboot: mapped [{:X}, {:X}) into its own aspace, entry at {:X}, stack {:X}",
              loaded.image.base, loaded.image.base + loaded.image.span, loaded.image.entry,
              loaded.stack_top);

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

    // Both images are placed, so the program's imports can be resolved against
    // the vDSO's exports. This has to happen before the thread starts and
    // after both load addresses are known, which is the only reason the
    // program is not simply bound as it is mapped.
    if (Status::Ok != bind_imports(image.unwrap(), loaded.image, vdso_bytes.unwrap(), vdso)) {
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
    //
    // No arguments. arg1 used to be the vDSO's export table, because that was
    // the only way a program could reach a system call; a program's imports
    // are bound before it runs now, so there is nothing it needs to be told at
    // entry. The pair stays in the struct because a process started with a
    // handle to its bootstrap channel is what goes here next -- which is what
    // Zircon's userboot receives.
    status = thread.dispatcher()->start({
        .entry = loaded.image.entry,
        .stack = loaded.stack_top,
        .arg1 = 0,
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
