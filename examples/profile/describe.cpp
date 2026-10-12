/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "describe.hpp"

#include <ours/object_info.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <span>
#include <string_view>

#include <cstring>

#include <eftl/binary_stream.hpp>
#include <eftl/elf/details/structures.hpp>
#include <eftl/elf/note.hpp>
#include <eftl/elf/traits.hpp>

namespace profile {
namespace {
    /// The unit the loader maps a segment in: whole pages.
    constexpr u64 LOAD_PAGE = 4096;

    /// The type of the note a linker's --build-id writes, under the owner name
    /// "GNU" (NT_GNU_BUILD_ID).
    constexpr u32 NOTE_GNU_BUILD_ID = 3;

    /// The most program headers read, and the most bytes of one note segment:
    /// a program linked here has about ten headers and one note, its build ID.
    constexpr usize MAX_HEADERS = 16;
    constexpr usize MAX_NOTES = 256;

    /// The most mappings one description reads. A process here has a few dozen;
    /// past this the rest are not looked at, and their modules not described.
    constexpr usize MAX_MAPS = 256;

    /// Static rather than on the stack: 24 KiB. One description runs at a time.
    ours::InfoMaps MAPS[MAX_MAPS];

    /// Reads exactly |into| from |process|'s memory at |address|: false when the
    /// read fails or comes back short, as at a mapping's end.
    auto read_exactly(ours::Uandle const process, u64 const address, void *const into,
                      usize const size) -> bool {
        usize actual = 0;
        return ours::process_read_memory(process, address, into, size, &actual) ==
                   ours::Status::Ok &&
               actual == size;
    }

    auto is_executable(ours::InfoMaps const &entry) -> bool {
        return entry.type == ours::InfoMapsType::Mapping && entry.u.mapping.vmo_koid != 0 &&
               (entry.u.mapping.mmu_flags & ours::VmOptions::PermExecute) != ours::VmOptions::None;
    }

    /// The mapping of VMO |vmo| at offset 0, where the loader put the ELF
    /// header, since it maps a program by segment out of one VMO; null when
    /// there is none.
    auto header_mapping(std::span<ours::InfoMaps const> const maps, ours::KoId const vmo)
        -> ours::InfoMaps const * {
        auto const found = std::ranges::find_if(maps, [vmo](ours::InfoMaps const &entry) {
            return entry.type == ours::InfoMapsType::Mapping && entry.u.mapping.vmo_koid == vmo &&
                   entry.u.mapping.vmo_offset == 0;
        });
        return found == maps.end() ? nullptr : &*found;
    }

    /// The VMO's name as markup may hold it: up to its NUL, and only up to a
    /// ':' or a '}', which would end a markup field.
    auto take_name(ours::InfoMaps const &entry, Module *const module) -> void {
        auto const name = std::string_view{entry.name, ours::MAX_NAME_LEN};
        auto const end = std::min(name.find('\0'), name.find_first_of(":}"));
        module->name_size = std::min(end, ours::MAX_NAME_LEN);
        std::memcpy(module->name.data(), entry.name, module->name_size);
    }

    /// The build ID in the note segment |header| describes, which lies at
    /// |bias| plus its address in |process|'s memory, into |module|; false when
    /// it holds none.
    auto read_build_id(ours::Uandle const process, u64 const bias,
                       eftl::elf::Elf64Phdr const &header, Module *const module) -> bool {
        static u8 NOTES[MAX_NOTES];
        auto const size = std::min<usize>(header.p_filesz, MAX_NOTES);
        if (!read_exactly(process, bias + header.p_vaddr, NOTES, size)) {
            return false;
        }
        auto const notes =
            eftl::elf::NoteSectionView<eftl::elf::Traits64>::from(eftl::ByteRange(NOTES, size));
        if (notes.is_none()) {
            return false;
        }
        for (usize index = 0; index < notes->size(); ++index) {
            auto const note = notes->note_at(index);
            if (note.is_none() || note->m_type != NOTE_GNU_BUILD_ID) {
                continue;
            }
            auto const owner = std::string_view{reinterpret_cast<char const *>(note->m_name.data()),
                                                note->m_name.size()};
            auto const id = note->m_descriptor;
            if (owner == std::string_view{"GNU", 4} && !id.empty() && id.size() <= MAX_BUILD_ID) {
                std::memcpy(module->build_id.data(), id.data(), id.size());
                module->build_id_size = id.size();
                return true;
            }
        }
        return false;
    }

    /// The module whose ELF header |header| maps, read from |process|: its
    /// segments from the program headers, moved by where the header was loaded,
    /// and its build ID. False when the bytes there are not an ELF with a build
    /// ID.
    ///
    /// The segment that holds file offset 0 says which program address the
    /// header was linked at; the header's address in memory less that is the
    /// bias every program address is offset by.
    auto read_module(ours::Uandle const process, ours::InfoMaps const &header, Module *const module)
        -> bool {
        using eftl::elf::Elf64Phdr;
        using eftl::elf::SegType;

        eftl::elf::Elf64Ehdr ehdr{};
        if (!read_exactly(process, header.base, &ehdr, sizeof(ehdr)) ||
            std::memcmp(ehdr.e_ident,
                        "\x7f"
                        "ELF",
                        4) != 0 ||
            ehdr.e_phentsize != sizeof(Elf64Phdr) || ehdr.e_phnum > MAX_HEADERS) {
            return false;
        }
        Elf64Phdr headers[MAX_HEADERS];
        auto const program_headers = std::span{headers}.first(ehdr.e_phnum);
        if (!read_exactly(process, header.base + ehdr.e_phoff, headers,
                          program_headers.size_bytes())) {
            return false;
        }
        auto const first = std::ranges::find_if(program_headers, [](Elf64Phdr const &segment) {
            return segment.type() == SegType::Load && segment.p_offset == 0;
        });
        if (first == program_headers.end()) {
            return false;
        }
        auto const bias = header.base - first->p_vaddr;

        module->num_segments = 0;
        module->build_id_size = 0;
        for (auto const &segment: program_headers) {
            if (segment.type() == SegType::Load && module->num_segments < MAX_SEGMENTS) {
                auto const begin = segment.p_vaddr & ~(LOAD_PAGE - 1);
                auto const end =
                    (segment.p_vaddr + segment.p_memsz + LOAD_PAGE - 1) & ~(LOAD_PAGE - 1);
                module->segments[module->num_segments++] = LoadedSegment{
                    .start = bias + begin,
                    .size = end - begin,
                    .permissions = segment.permissions(),
                    .relative = begin,
                };
            } else if (segment.type() == SegType::Note && module->build_id_size == 0) {
                auto _ = read_build_id(process, bias, segment, module);
            }
        }
        return module->build_id_size != 0;
    }
} // namespace

auto Module::runs(u64 const address) const -> bool {
    return std::ranges::any_of(std::span{segments}.first(num_segments),
                               [address](LoadedSegment const &segment) {
                                   return (segment.permissions & eftl::elf::SegPerms::Executable) !=
                                              eftl::elf::SegPerms::None &&
                                          address - segment.start < segment.size;
                               });
}

auto Process::runs(u64 const address) const -> bool {
    return std::ranges::any_of(std::span{modules}.first(num_modules),
                               [address](Module const &module) { return module.runs(address); });
}

/// Every executable mapping names its VMO; the first of them for a VMO not
/// seen yet makes a module, read through that VMO's mapping at offset 0.
/// The maps are a snapshot: a mapping made after it is not described.
auto describe(ours::Uandle const handle, ours::KoId const koid, Process *const out)
    -> ours::Status {
    usize actual = 0;
    if (auto const status = ours::object_get_info(handle, ours::InfoTopic::ProcessMaps, MAPS,
                                                  sizeof(MAPS), &actual, nullptr);
        status != ours::Status::Ok) {
        return status;
    }
    auto const maps = std::span<ours::InfoMaps const>{MAPS, actual};

    out->koid = koid;
    out->num_modules = 0;
    for (auto const &entry: maps) {
        if (!is_executable(entry) || out->num_modules == MAX_MODULES) {
            continue;
        }
        auto const vmo = entry.u.mapping.vmo_koid;
        auto const modules = std::span{out->modules}.first(out->num_modules);
        if (std::ranges::any_of(modules,
                                [vmo](Module const &module) { return module.vmo == vmo; })) {
            continue;
        }
        auto const *const header = header_mapping(maps, vmo);
        auto &module = out->modules[out->num_modules];
        module.vmo = vmo;
        if (header != nullptr && read_module(handle, *header, &module)) {
            take_name(*header, &module);
            ++out->num_modules;
        }
    }
    return ours::Status::Ok;
}
} // namespace profile
