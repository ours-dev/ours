/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_NOTE_HPP
#define EFTL_ELF_NOTE_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/details/structures.hpp>
#include <eftl/elf/encoding.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>

#include <cstring>

#include <ustl/option.hpp>

namespace eftl::elf {
struct NoteRecord final {
    u32 m_type{0};
    ByteRange m_name{};
    ByteRange m_descriptor{};
};

template <typename Traits>
class NoteSectionView final {
  public:
    static_assert(sizeof(typename Traits::Addr) == 4 || sizeof(typename Traits::Addr) == 8,
                  "expected an ELF32 or ELF64 trait");

    [[nodiscard]]
    static auto from(ByteRange const bytes, ByteOrder const byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<NoteSectionView> {
        if (!bytes.empty() && bytes.data() == nullptr) {
            return ustl::NONE;
        }

        usize offset = 0;
        usize count = 0;
        while (offset != bytes.size()) {
            auto const parsed = parse_at(bytes, offset, byte_order);
            if (parsed.is_none() || count == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            offset = parsed->m_next_offset;
            ++count;
        }
        return ustl::some(NoteSectionView(bytes, count, byte_order));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto note_at(usize const index) const noexcept -> ustl::Option<NoteRecord> {
        if (index >= m_count) {
            return ustl::NONE;
        }

        usize offset = 0;
        for (usize current = 0; current <= index; ++current) {
            auto const parsed = parse_at(m_bytes, offset, m_byte_order);
            if (parsed.is_none()) {
                return ustl::NONE;
            }
            if (current == index) {
                return ustl::some(NoteRecord{parsed->m_record});
            }
            offset = parsed->m_next_offset;
        }
        return ustl::NONE;
    }

  private:
    struct ParsedNote final {
        NoteRecord m_record{};
        usize m_next_offset{0};
    };

    constexpr NoteSectionView(ByteRange const bytes, usize const count,
                              ByteOrder const byte_order) noexcept
        : m_bytes(bytes), m_count(count), m_byte_order(byte_order) {}

    [[nodiscard]]
    static constexpr auto align_word(usize const value) noexcept -> ustl::Option<usize> {
        // GNU/Linux note sections (e.g. .note.gnu.build-id) are always
        // 4-byte aligned regardless of ELF class; the strict ELF64 gABI's
        // 8-byte word alignment is not what real toolchains emit.
        constexpr usize ALIGNMENT = 4;
        if (value > std::numeric_limits<usize>::max() - (ALIGNMENT - 1)) {
            return ustl::NONE;
        }
        return ustl::some((value + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    }

    [[nodiscard]]
    static constexpr auto end_offset(usize const begin, usize const size,
                                     usize const limit) noexcept -> ustl::Option<usize> {
        if (begin > limit || size > limit - begin) {
            return ustl::NONE;
        }
        return ustl::some(begin + size);
    }

    [[nodiscard]]
    static auto parse_at(ByteRange const bytes, usize const offset,
                         ByteOrder const byte_order) noexcept -> ustl::Option<ParsedNote> {
        auto const header_end = end_offset(offset, sizeof(ElfNoteHeader), bytes.size());
        if (header_end.is_none()) {
            return ustl::NONE;
        }

        auto const *header_data = bytes.data() + offset;
        ElfNoteHeader header{};
        if (byte_order == ByteOrder::Little) {
            header = RecordCodec<Traits, ElfNoteHeader>::decode_little(header_data);
        } else {
            header = RecordCodec<Traits, ElfNoteHeader>::decode_big(header_data);
        }
        auto const name_end =
            end_offset(header_end.value(), static_cast<usize>(header.n_namesz), bytes.size());
        if (name_end.is_none()) {
            return ustl::NONE;
        }
        auto const descriptor_offset = align_word(name_end.value());
        if (descriptor_offset.is_none() || descriptor_offset.value() > bytes.size()) {
            return ustl::NONE;
        }
        auto const descriptor_end = end_offset(descriptor_offset.value(),
                                               static_cast<usize>(header.n_descsz), bytes.size());
        if (descriptor_end.is_none()) {
            return ustl::NONE;
        }
        auto const next_offset = align_word(descriptor_end.value());
        if (next_offset.is_none() || next_offset.value() > bytes.size()) {
            return ustl::NONE;
        }

        NoteRecord record{};
        record.m_type = header.n_type;
        record.m_name =
            ByteRange(bytes.data() + header_end.value(), static_cast<usize>(header.n_namesz));
        record.m_descriptor = ByteRange(bytes.data() + descriptor_offset.value(),
                                        static_cast<usize>(header.n_descsz));
        return ustl::some(ParsedNote{.m_record = record, .m_next_offset = next_offset.value()});
    }

    ByteRange m_bytes{};
    usize m_count{0};
    ByteOrder m_byte_order{ByteOrder::Little};
};
} // namespace eftl::elf

#endif // EFTL_ELF_NOTE_HPP
