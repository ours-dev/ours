/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_GNU_PROPERTY_HPP
#define EFTL_ELF_GNU_PROPERTY_HPP 1

#include <eftl/elf/note.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
struct GnuPropertyRecord final {
    u32 m_type{0};
    ByteRange m_data{};
};

template <typename Traits>
class GnuPropertyNoteView final {
  public:
    [[nodiscard]]
    static constexpr auto is_gnu_property_note(NoteRecord const &note) noexcept -> bool {
        return note.m_type == NOTE_TYPE && note.m_name.size() == sizeof(GNU_NAME) &&
               note.m_name.data() != nullptr && note.m_name.data()[0] == 'G' &&
               note.m_name.data()[1] == 'N' && note.m_name.data()[2] == 'U' &&
               note.m_name.data()[3] == 0;
    }

    [[nodiscard]]
    static auto from(NoteRecord const &note, ByteOrder byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<GnuPropertyNoteView> {
        if (!is_gnu_property_note(note) ||
            (!note.m_descriptor.empty() && note.m_descriptor.data() == nullptr)) {
            return ustl::NONE;
        }

        auto offset = usize{0};
        auto count = usize{0};
        auto previous_type = u32{0};
        auto has_previous = false;
        while (offset != note.m_descriptor.size()) {
            auto const parsed = parse_at(note.m_descriptor, offset, byte_order);
            if (parsed.is_none() || count == std::numeric_limits<usize>::max() ||
                (has_previous && parsed->m_record.m_type <= previous_type)) {
                return ustl::NONE;
            }
            previous_type = parsed->m_record.m_type;
            has_previous = true;
            offset = parsed->m_next_offset;
            ++count;
        }
        return ustl::some(GnuPropertyNoteView(note.m_descriptor, count, byte_order));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto property_at(usize index) const noexcept -> ustl::Option<GnuPropertyRecord> {
        if (index >= m_count) {
            return ustl::NONE;
        }

        auto offset = usize{0};
        for (usize current = 0; current <= index; ++current) {
            auto const parsed = parse_at(m_descriptor, offset, m_byte_order);
            if (parsed.is_none()) {
                return ustl::NONE;
            }
            if (current == index) {
                return ustl::some(GnuPropertyRecord{parsed->m_record});
            }
            offset = parsed->m_next_offset;
        }
        return ustl::NONE;
    }

  private:
    static constexpr u32 NOTE_TYPE = 5;
    static constexpr u8 GNU_NAME[] = {'G', 'N', 'U', 0};
    static constexpr usize PROPERTY_HEADER_SIZE = 2 * sizeof(u32);

    struct ParsedProperty final {
        GnuPropertyRecord m_record{};
        usize m_next_offset{0};
    };

    constexpr GnuPropertyNoteView(ByteRange descriptor, usize count, ByteOrder byte_order) noexcept
        : m_descriptor(descriptor), m_count(count), m_byte_order(byte_order) {}

    [[nodiscard]]
    static constexpr auto align_word(usize value) noexcept -> ustl::Option<usize> {
        constexpr usize ALIGNMENT = sizeof(typename Traits::Addr);
        if (value > std::numeric_limits<usize>::max() - (ALIGNMENT - 1)) {
            return ustl::NONE;
        }
        return ustl::some((value + ALIGNMENT - 1) & ~(ALIGNMENT - 1));
    }

    [[nodiscard]]
    static constexpr auto end_offset(usize begin, usize size, usize limit) noexcept
        -> ustl::Option<usize> {
        if (begin > limit || size > limit - begin) {
            return ustl::NONE;
        }
        return ustl::some(begin + size);
    }

    [[nodiscard]]
    static auto parse_at(ByteRange descriptor, usize offset, ByteOrder byte_order) noexcept
        -> ustl::Option<ParsedProperty> {
        auto const header_end = end_offset(offset, PROPERTY_HEADER_SIZE, descriptor.size());
        if (header_end.is_none()) {
            return ustl::NONE;
        }
        auto const *data = descriptor.data() + offset;
        auto type = u32{0};
        if (byte_order == ByteOrder::Little) {
            type = ScalarCodec<u32>::decode_little(data);
        } else {
            type = ScalarCodec<u32>::decode_big(data);
        }
        auto data_size = u32{0};
        if (byte_order == ByteOrder::Little) {
            data_size = ScalarCodec<u32>::decode_little(data + sizeof(u32));
        } else {
            data_size = ScalarCodec<u32>::decode_big(data + sizeof(u32));
        }
        auto const data_end =
            end_offset(header_end.value(), static_cast<usize>(data_size), descriptor.size());
        ustl::Option<usize> next_offset = ustl::NONE;
        if (data_end.is_some()) {
            next_offset = align_word(data_end.value());
        }
        if (next_offset.is_none() || next_offset.value() > descriptor.size()) {
            return ustl::NONE;
        }
        return ustl::some(ParsedProperty{
            .m_record =
                GnuPropertyRecord{
                    .m_type = type,
                    .m_data = ByteRange(descriptor.data() + header_end.value(),
                                        static_cast<usize>(data_size)),
                },
            .m_next_offset = next_offset.value(),
        });
    }

    ByteRange m_descriptor{};
    usize m_count{0};
    ByteOrder m_byte_order{ByteOrder::Little};
};
} // namespace eftl::elf

#endif // EFTL_ELF_GNU_PROPERTY_HPP
