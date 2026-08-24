/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_GROUP_HPP
#define EFTL_ELF_GROUP_HPP 1

#include <eftl/elf/encoding.hpp>
#include <eftl/elf/table.hpp>

#include <limits>

#include <ustl/option.hpp>

namespace eftl::elf {
class SectionGroupView final {
  public:
    using Word = u32;
    using Words = TableView<Word>;

    [[nodiscard]]
    static auto from(ByteRange const entries, usize const signature_symbol_index,
                     usize const section_count, usize const group_section_index,
                     ByteOrder const byte_order = ByteOrder::Little) noexcept
        -> ustl::Option<SectionGroupView> {
        if (signature_symbol_index == 0 || section_count == 0 || entries.size() < sizeof(Word) ||
            entries.size() % sizeof(Word) != 0) {
            return ustl::NONE;
        }
        typename Words::Decoder decoder;
        if (byte_order == ByteOrder::Little) {
            decoder = &ScalarCodec<Word>::decode_little;
        } else {
            decoder = &ScalarCodec<Word>::decode_big;
        }
        auto const words =
            Words::from_checked_bytes(entries, entries.size() / sizeof(Word), decoder);
        if (words.is_none()) {
            return ustl::NONE;
        }
        for (usize index = 1; index < words->size(); ++index) {
            auto const member = to_usize((*words)[index]);
            if (member.is_none() || member.value() == 0 || member.value() >= section_count ||
                member.value() == group_section_index) {
                return ustl::NONE;
            }
            for (usize earlier = 1; earlier < index; ++earlier) {
                if ((*words)[earlier] == (*words)[index]) {
                    return ustl::NONE;
                }
            }
        }
        return ustl::some(SectionGroupView(words.value(), signature_symbol_index));
    }

    [[nodiscard]]
    constexpr auto flags() const noexcept -> Word {
        return m_words[0];
    }

    [[nodiscard]]
    constexpr auto is_comdat() const noexcept -> bool {
        return (flags() & K_COMDAT) != 0;
    }

    [[nodiscard]]
    constexpr auto signature_symbol_index() const noexcept -> usize {
        return m_signature_symbol_index;
    }

    [[nodiscard]]
    constexpr auto member_count() const noexcept -> usize {
        return m_words.size() - 1;
    }

    [[nodiscard]]
    auto member_section_at(usize const index) const noexcept -> ustl::Option<usize> {
        if (index >= member_count()) {
            return ustl::NONE;
        }
        auto const member = m_words.at(index + 1);
        if (member.is_none()) {
            return ustl::NONE;
        }
        return to_usize(member.value());
    }

  private:
    static constexpr Word K_COMDAT = 1;

    constexpr SectionGroupView(Words const words, usize const signature_symbol_index) noexcept
        : m_words(words), m_signature_symbol_index(signature_symbol_index) {}

    [[nodiscard]]
    static constexpr auto to_usize(Word const value) noexcept -> ustl::Option<usize> {
        if constexpr (sizeof(Word) > sizeof(usize)) {
            if (value > static_cast<Word>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    Words m_words;
    usize m_signature_symbol_index{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_GROUP_HPP
