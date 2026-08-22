#include <ours/phys/build-id-note.hpp>

#include <cstring>

namespace ours::phys {
namespace {
    struct NoteHeader {
        u32 name_size;
        u32 desc_size;
        u32 type;
    };
    static_assert(sizeof(NoteHeader) == 12);

    constexpr u32 kGnuBuildId = 3; // NT_GNU_BUILD_ID

    // The universal GNU/Linux note convention.  The strict ELF64 gABI calls
    // for 8-byte ("native word") alignment, but GNU ld's --build-id and
    // every consumer of it (readelf, gdb, the kernel) use 4 regardless of
    // ELF class; a real build-id note is laid out accordingly.
    constexpr usize kNoteAlign = 4;

    constexpr auto align_up(usize const value, usize const alignment) -> usize {
        return (value + alignment - 1) & ~(alignment - 1);
    }
} // namespace

auto hex_encode_build_id(std::span<u8 const> const note_section, std::span<char> const out)
    -> usize {
    if (note_section.size() < sizeof(NoteHeader)) {
        return 0;
    }
    NoteHeader header;
    std::memcpy(&header, note_section.data(), sizeof(header));

    auto const name_offset = sizeof(header);
    auto const name_end = name_offset + header.name_size;
    if (header.name_size != 4 || name_end > note_section.size() ||
        std::memcmp(note_section.data() + name_offset, "GNU", 3) != 0 ||
        note_section[name_offset + 3] != u8{0}) {
        return 0;
    }

    auto const desc_offset = align_up(name_end, kNoteAlign);
    auto const desc_end = desc_offset + header.desc_size;
    if (desc_offset < name_end || desc_end < desc_offset || desc_end > note_section.size()) {
        return 0;
    }
    // Exactly one note: nothing follows but this note's own trailing
    // alignment padding, matching what a single `ld --build-id` note
    // actually produces.  A section with a second note (or trailing junk)
    // is rejected rather than silently reading only the first.
    if (align_up(desc_end, kNoteAlign) != align_up(note_section.size(), kNoteAlign)) {
        return 0;
    }
    if (header.type != kGnuBuildId) {
        return 0;
    }

    auto const length = static_cast<usize>(header.desc_size) * 2;
    if (length == 0 || length >= out.size()) {
        return 0;
    }
    constexpr char kHexDigits[] = "0123456789abcdef";
    for (usize index = 0; index < header.desc_size; ++index) {
        auto const value = note_section[desc_offset + index];
        out[index * 2] = kHexDigits[value >> 4];
        out[index * 2 + 1] = kHexDigits[value & 0xF];
    }
    out[length] = '\0';
    return length;
}

} // namespace ours::phys
