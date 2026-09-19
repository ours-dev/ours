/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <elfload/loader.hpp>

#include <algorithm>

#include <cstring>

#include <logz4/log.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ustl/mem/align.hpp>

namespace elfload {
using namespace ours;

namespace {
    /// The 64-bit ELF header and program header, as far as loading needs
    /// them.
    ///
    /// Declared here rather than reached for from //kernel/lib/eftl, which this
    /// library already depends on for the binding half. Placement asks two
    /// questions of a program header -- where does this segment go, and how much
    /// of it is in the file -- and answering them directly is two dozen lines of
    /// arithmetic that can be read top to bottom; eftl reaches the same fields
    /// through traits-parameterised views built for the harder job beside it.
    ///
    /// That trade stops being worth it the moment segments are mapped with their
    /// own permissions: eftl's LoadPolicy::make_plan() already yields one
    /// LoadRegion per segment and the PT_GNU_RELRO interval with it, which is
    /// exactly what that needs (docs/repairs/260919/elfload.md).
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
} // namespace

auto Loader::map(Bytes const bytes, mm::VmPlacement const where, mm::MmuFlags const flags,
                 char const *const name, Image &out) const -> Status {
    if (bytes.size() < sizeof(Elf64Header)) {
        log::error("elfload: {} is too small to be an ELF", name);
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
            log::error("elfload: {} program header {} runs past the image", name, index);
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
        log::error("elfload: {} has no PT_LOAD segments", name);
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
        log::error("elfload: could not allocate {} frames for {}", nr_frames, name);
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
            log::error("elfload: {} segment {} content runs past the image", name, index);
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
    auto const status = object::VmoDispatcher::create_physical(mm::PhysMap::virt_to_phys(backing),
                                                               out.span, vmo, vmo_rights);
    if (Status::Ok != status) {
        log::error("elfload: could not create the {} VMO: {}", name, to_string(status));
        return status;
    }

    // Through the dispatcher, not through the VmArea it stands for: a
    // VmArea knows what a mapping is allowed to be, but cannot know who is
    // asking, and every future sys_vmar_map has to answer that. Rights
    // live on the handle, so the entry point has to be on the object that
    // a handle names.
    auto mapping =
        m_root_vma.map(*vmo.dispatcher(), where, out.span, flags, mm::VmMapOption::Commit, name);
    if (mapping.is_err()) {
        log::error("elfload: could not map {}: {}", name, to_string(mapping.unwrap_err()));
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
} // namespace elfload
