/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <kelfload/loader.hpp>

#include <algorithm>
#include <string_view>

#include <cstring>

#include <elfload/program.hpp>
#include <logz4/log.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/object/vmo_dispatcher.hpp>
#include <ours/vma.hpp>
#include <ustl/mem/align.hpp>

namespace kelfload {
using namespace ours;
using elfload::Image;

namespace {
    /// The 64-bit ELF header and program header, as far as loading needs
    /// them.
    ///
    /// Declared here rather than reached for from //system/lib/eftl, which this
    /// library already depends on for the binding half. Placement asks two
    /// questions of a program header -- where does this segment go, and how much
    /// of it is in the file -- and answering them directly is two dozen lines of
    /// arithmetic that can be read top to bottom; eftl reaches the same fields
    /// through traits-parameterised views built for the harder job beside it.
    ///
    /// Mapping segments with their own permissions is where that trade stops
    /// being worth it, and map_segments() does not read these: it takes its
    /// mappings from elfload::mapping_pages(), which asks eftl's
    /// LoadPolicy::make_plan() at page granularity.
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

    /// The ImageWriter for an image backed by a VMO: |context| is the
    /// mm::VmObject, and |offset| is already an offset into it, because the
    /// VMO starts at the image's lowest address.
    auto write_through_vmo(void *const context, usize const offset, void const *const bytes,
                           usize const size) -> Status {
        return static_cast<mm::VmObject *>(context)->write(bytes, offset, size);
    }

    /// How a debug line spells |mapping|'s protection. Every mapping is
    /// readable (elfload::MappingPages).
    auto protection_of(elfload::MappingPages const &mapping) -> std::string_view {
        if (mapping.write) {
            return "rw-";
        }
        if (mapping.execute) {
            return "r-x";
        }
        if (mapping.relro) {
            return "r-- (RELRO)";
        }
        return "r--";
    }
} // namespace

auto Loader::load(Bytes const bytes, char const *const name, ImageVmo &out) -> Status {
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
    auto lowest = VirtAddr{~0_usize};
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

    // Whole pages: a VMO commits nothing smaller, and a mapping cannot start
    // or end mid-page either.
    lowest = ustl::round_down(lowest, PAGE_SIZE);
    highest = ustl::round_up(highest, PAGE_SIZE);
    out.span = highest - lowest;

    // A paged VMO, whole and resident from the start. Two steps,
    // deliberately: first say what the memory *is*, then say where it
    // appears. That split is the whole reason a VMO is a separate object from
    // a mapping -- it is what lets the same memory be mapped twice, and what
    // makes "hand a program some memory" a different operation from "put it
    // in that program's address space".
    //
    // This used to take a run of frames with get_frames_n(), fill them, and
    // wrap them in a *physical* VMO. A physical VMO does not own its frames
    // -- it is a window onto memory someone else keeps alive -- so nothing
    // ever gave them back: every image loaded leaked all of its pages, and a
    // process could not be torn down without leaking them for good (step K6
    // of docs/design/260924/userspace-filesystem.md needs them returned).
    // Paged is also simply what these are: ordinary anonymous pages, which
    // the VMO now owns and frees with itself.
    //
    // Pinned, and so committed here, for the reason create_paged() gives:
    // nothing services a fault on it yet.
    auto &vmo = out.vmo;
    Rights vmo_rights;
    if (auto const status = object::VmoDispatcher::create_paged(out.span, vmo, vmo_rights);
        Status::Ok != status) {
        log::error("elfload: could not create the {} VMO: {}", name, to_string(status));
        return status;
    }
    // A reference of its own: the dispatcher hands its VMO out read-only.
    auto memory = vmo.dispatcher()->vmo();

    // A new VMO reads as zero everywhere, which is what gives .bss its
    // zeroes: the gap between filesz and memsz is never written. Each
    // segment's file bytes then go to its offset from the lowest address --
    // the same layout as before, through the VMO instead of a pointer.
    for (auto index = u16(0); index < header.phnum; ++index) {
        Elf64ProgramHeader phdr{};
        std::memcpy(&phdr, bytes.data() + header.phoff + usize(index) * header.phentsize,
                    sizeof(phdr));
        if (phdr.type != PT_LOAD || phdr.filesz == 0) {
            continue;
        }
        if (phdr.offset > bytes.size() || phdr.filesz > bytes.size() - phdr.offset) {
            log::error("elfload: {} segment {} content runs past the image", name, index);
            return Status::InvArgs;
        }
        // A segment whose file bytes run past its own place in the span
        // (filesz > memsz, or a vaddr the first pass did not see) is refused
        // by the VMO's own range check rather than written short.
        if (auto const status = memory->write(bytes.data() + phdr.offset,
                                              VirtAddr(phdr.vaddr) - lowest, phdr.filesz);
            Status::Ok != status) {
            log::error("elfload: {} segment {} does not fit its image: {}", name, index,
                       to_string(status));
            return status;
        }
    }

    out.image_base = lowest;
    out.entry_offset = VirtAddr(header.entry) - lowest;
    return Status::Ok;
}

auto Loader::map_segments(ImageVmo &image, Bytes const file, VirtAddr const at,
                          VmOptions const extra, char const *const name,
                          elfload::Diagnostics const &diagnostics, Image &out) const -> Status {
    elfload::MappingPagesList mappings;
    if (auto const status = elfload::mapping_pages(diagnostics, file, mappings);
        Status::Ok != status) {
        log::error("elfload: {} cannot be mapped by segment", name);
        return status;
    }

    auto &vmo = image.vmo;
    auto memory = vmo.dispatcher()->vmo();
    for (auto const &mapping: mappings) {
        // Through the dispatcher, not through the VmArea it stands for: a
        // VmArea knows what a mapping is allowed to be, but cannot know who is
        // asking, and every syscall::vma_map has to answer that. Rights live
        // on the handle, so the entry point has to be on the object that a
        // handle names.
        //
        // At its place exactly, populated now, readable always (why is on
        // elfload::MappingPages), and never above its own protection: each
        // Perm bit brings its CanMap bit and nothing else, so nothing the
        // program does later raises a mapping above what its p_flags say.
        auto options = extra | VmOptions::PermRead | VmOptions::CanMapRead | VmOptions::Specific |
                       VmOptions::MapRange;
        if (mapping.write) {
            options |= VmOptions::PermWrite | VmOptions::CanMapWrite;
        }
        if (mapping.execute) {
            options |= VmOptions::PermExecute | VmOptions::CanMapExecute;
        }
        // The VMO starts at the image's first page, and so does |at|: a
        // mapping is at the same offset in both.
        auto const base = at + mapping.offset;
        auto const mapped = m_root_vma.map(options, base - m_root_vma.vma()->base(),
                                           *vmo.dispatcher(), mapping.offset, mapping.size, name);
        if (mapped.is_err()) {
            log::error("elfload: could not map {} at +{:#x}: {}", name, mapping.offset,
                       to_string(mapped.unwrap_err()));
            return mapped.unwrap_err();
        }
        log::debug("elfload: {} [{:X}, {:X}) {}", name, base, base + mapping.size,
                   protection_of(mapping));
    }

    out.base = at;
    out.span = image.span;
    // The Binder writes relocations through the VMO as well. The pointer
    // stays valid for as long as the image is mapped: every mapping holds its
    // own reference to the VMO, so the object outlives |memory|, dropped at
    // the end of this function, and the handle in |image|, which a caller may
    // drop as soon as this returns.
    out.writer = {.write = write_through_vmo, .context = memory.as_ptr_mut()};
    out.image_base = image.image_base;
    // e_entry is a file-relative address for a PIE, so it moves with the
    // load base like everything else. It is the first instruction of a
    // program; for the vDSO it means nothing at all -- nothing jumps to a
    // vDSO -- and is recorded here only because one routine maps both.
    out.entry = out.base + image.entry_offset;
    return Status::Ok;
}
} // namespace kelfload
