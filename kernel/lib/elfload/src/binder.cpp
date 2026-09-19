/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <elfload/binder.hpp>

#include <type_traits>

#include <eftl/elf/dynamic_image.hpp>
#include <eftl/elf/mapped_memory.hpp>
#include <eftl/elf/parser.hpp>
#include <logz4/log.hpp>

namespace elfload {
using ours::Status;

namespace {
    /// The x86-64 relocation types this binder applies.
    ///
    /// Four, and no more, because of how the images are built: a program is
    /// compiled -fno-plt and linked -z now, so an imported function reaches
    /// the GOT the way an imported variable does (GLOB_DAT), and there is no
    /// lazily-bound stub to service. JUMP_SLOT is accepted anyway because a
    /// linker may still emit one for an address taken rather than called, and
    /// under BIND_NOW it means the same thing: store the resolved address.
    /// RELATIVE is the image's own absolute pointers; R_X86_64_64 is a
    /// symbol's address stored whole.
    ///
    /// Anything else is refused rather than ignored. A relocation left
    /// unapplied is a pointer that is quietly wrong, and the fault it causes
    /// points at the use, never at the cause.
    constexpr u32 R_X86_64_64 = 1;
    constexpr u32 R_X86_64_GLOB_DAT = 6;
    constexpr u32 R_X86_64_JUMP_SLOT = 7;
    constexpr u32 R_X86_64_RELATIVE = 8;

    /// SHN_UNDEF: the symbol is imported, not defined, by the image whose
    /// table it is in.
    constexpr u16 SHN_UNDEF = 0;

    /// Whether |dynamic| names |tag| at all, readable or not.
    ///
    /// Asked of the two relocation tables because eftl answers NONE both for
    /// "this image has no such table" and for "it has one I could not read",
    /// and only the first is harmless. Treating the second as the first is
    /// how a .rela.plt once went unapplied without a word: the GOT kept its
    /// link-time values and the first call through it jumped to an
    /// unrelocated address in ring 3, far from anything that could say why.
    template <typename Dynamic>
    [[nodiscard]]
    auto declares(Dynamic const &dynamic, eftl::elf::DynTag const tag) -> bool {
        for (auto const entry: dynamic.entries()) {
            if (static_cast<i64>(entry.d_tag) == static_cast<i64>(tag)) {
                return true;
            }
        }
        return false;
    }
} // namespace

auto Binder::from(Bytes const file, Image const &image) -> Result {
    auto binary = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (binary.is_err()) {
        log::error("elfload: the exporter is not an ELF this loader can read");
        return ustl::err(Status::InvArgs);
    }

    auto const dynamic = binary->dynamic_image();
    if (dynamic.is_none()) {
        log::error("elfload: the exporter has no PT_DYNAMIC, so it exports nothing");
        return ustl::err(Status::InvArgs);
    }

    auto const exports = dynamic->dynamic_symbols();
    if (exports.is_none()) {
        log::error("elfload: the exporter has no readable .dynsym");
        return ustl::err(Status::InvArgs);
    }

    return ustl::ok(Binder(exports.value(), static_cast<u64>(image.base - image.image_base)));
}

auto Binder::resolve(std::string_view const name) const -> ustl::Option<u64> {
    for (usize index = 0; index < m_exports.size(); ++index) {
        auto const spelling = m_exports.symbol_name(index);
        if (spelling.is_none()) {
            continue;
        }
        auto const text =
            std::string_view(reinterpret_cast<char const *>(spelling->data()), spelling->size());
        if (text != name) {
            continue;
        }

        auto const symbol = m_exports.symbol_at(index);
        // The exporter imports this name too rather than defining it, so
        // binding to it would bind to nothing.
        if (symbol.is_none() || symbol->st_shndx == SHN_UNDEF) {
            continue;
        }
        return ustl::some(static_cast<u64>(symbol->st_value));
    }
    return ustl::NONE;
}

auto Binder::bind(Bytes const file, Image const &importer) const -> Status {
    auto binary = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (binary.is_err()) {
        log::error("elfload: the importer is not an ELF this loader can read");
        return Status::InvArgs;
    }

    auto const dynamic = binary->dynamic_image();
    if (dynamic.is_none()) {
        // A static image: nothing to bind, which is a legal shape.
        return Status::Ok;
    }

    auto const relas = dynamic->dynamic_rela_table();
    auto const plt_relas = dynamic->dynamic_plt_rela_table();

    // A table the image declares and eftl could not produce is refused, not
    // skipped: skipping it leaves every pointer in it at its link-time value.
    if (relas.is_none() && declares(dynamic.value(), eftl::elf::DynTag::Rela)) {
        log::error("elfload: the importer declares DT_RELA but it cannot be read");
        return Status::InvArgs;
    }
    if (plt_relas.is_none() && declares(dynamic.value(), eftl::elf::DynTag::JmpRel)) {
        log::error("elfload: the importer declares DT_JMPREL but it cannot be read");
        return Status::InvArgs;
    }

    // Two tags further along, the same shape. DT_REL and DT_RELR are
    // relocation formats this binder does not apply at all, and saying nothing
    // about them is a51f4cd's failure wearing a different tag: there, a table
    // eftl could not read was taken for a table that was not there, the GOT
    // kept its link-time fill, and the first call through it left ring 3 for
    // an address nothing had written. Skipping a format outright ends the same
    // way. `-Wl,-z,pack-relative-relocs` is all it would take -- it moves every
    // RELATIVE entry into .relr.dyn -- and such an image would load, bind
    // "successfully", and run with link-time pointers.
    //
    // glibc's elf_dynamic_do_Rel() walks whichever of the three an image
    // declares. Until this one does, it has to refuse what it cannot apply.
    if (declares(dynamic.value(), eftl::elf::DynTag::Rel)) {
        log::error("elfload: the importer declares DT_REL, which this binder does not apply");
        return Status::Unsupported;
    }
    if (declares(dynamic.value(), eftl::elf::DynTag::Relr)) {
        log::error("elfload: the importer declares DT_RELR, which this binder does not apply");
        return Status::Unsupported;
    }

    if (relas.is_none() && plt_relas.is_none()) {
        return Status::Ok;
    }

    auto const symbols = dynamic->dynamic_symbols();
    if (symbols.is_none()) {
        // Relocations that name symbols, and no table to name them from.
        log::error("elfload: the importer has relocations but no readable .dynsym");
        return Status::InvArgs;
    }

    // One region, because the Loader lays an image out as one: every link-time
    // address from image_base to image_base + span appears at the same offset
    // in |view|. A loader that gave each segment its own mapping would build
    // one region per segment instead; the translation eftl does is the same
    // either way.
    eftl::elf::LoadRegion const region{
        .virtual_start = importer.image_base,
        .virtual_end = importer.image_base + importer.span,
        .image_offset = 0,
        .permissions = eftl::elf::SegPerms::Readable | eftl::elf::SegPerms::Writable,
    };
    eftl::elf::MappedMemory<Traits> memory(importer.view, importer.span, {&region, 1});

    auto const bias = static_cast<u64>(importer.base - importer.image_base);

    auto apply = [&](auto const &table) -> Status {
        using Table = std::remove_cvref_t<decltype(table)>;

        for (usize index = 0; index < table.size(); ++index) {
            auto const entry = table.relocation_at(index);
            if (entry.is_none()) {
                return Status::InvArgs;
            }
            auto const &rela = entry.value();
            auto const type = Table::type(rela);
            auto const symbol_index = Table::symbol_index(rela);

            u64 value = 0;
            if (type == R_X86_64_RELATIVE) {
                // The image's own absolute pointers: no symbol involved, just
                // "wherever this image landed".
                value = bias + static_cast<u64>(rela.r_addend);
            } else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
                       type == R_X86_64_JUMP_SLOT) {
                auto const symbol = symbols->symbol_at(static_cast<usize>(symbol_index));
                auto const name = symbols->symbol_name(static_cast<usize>(symbol_index));
                if (symbol.is_none() || name.is_none()) {
                    return Status::InvArgs;
                }
                auto const spelling =
                    std::string_view(reinterpret_cast<char const *>(name->data()), name->size());

                if (symbol->st_shndx != SHN_UNDEF) {
                    // Defined in the importer itself. -Bsymbolic should have
                    // resolved it at link time, so reaching here means the
                    // linker left one for the loader after all.
                    value =
                        bias + static_cast<u64>(symbol->st_value) + static_cast<u64>(rela.r_addend);
                } else {
                    auto const definition = resolve(spelling);
                    if (definition.is_none()) {
                        log::error("elfload: '{}' is imported but the exporter does not define it",
                                   spelling);
                        return Status::NotFound;
                    }
                    value = m_bias + definition.value() + static_cast<u64>(rela.r_addend);
                    // Info, not debug. A ring-3 fault through an unbound or
                    // wrongly bound slot lands far from anything that can say
                    // why -- the only witness is a register dump with no
                    // symbols -- and when exactly that happened, neither image
                    // printed a single bound value, because debug is below the
                    // floor in both. What a loader resolved is a handful of
                    // lines per program and it is the first thing the next
                    // such dump needs beside it.
                    log::info("elfload: bound '{}' to {:X}", spelling, value);
                }
            } else {
                log::error("elfload: relocation type {} is one this binder does not apply", type);
                return Status::Unsupported;
            }

            // r_offset is a link-time address, which MappedMemory translates
            // through the region above into an offset in |view|. Adding the
            // bias here as well would double-count the load address and push
            // the write outside the image.
            if (!memory.write<u64>(static_cast<u64>(rela.r_offset), value)) {
                log::error("elfload: relocation {} writes outside the image", index);
                return Status::OutOfRange;
            }
        }
        return Status::Ok;
    };

    if (relas.is_some()) {
        if (auto const status = apply(relas.value()); status != Status::Ok) {
            return status;
        }
    }
    if (plt_relas.is_some()) {
        if (auto const status = apply(plt_relas.value()); status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}
} // namespace elfload
