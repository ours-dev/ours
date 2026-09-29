/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_TABLE_HPP
#define EFTL_ELF_TABLE_HPP 1

#include <eftl/binary_stream.hpp>

#include <limits>
#include <type_traits>

#include <cstring>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Entry>
class TableView final {
    static_assert(std::is_trivially_copyable_v<Entry>,
                  "ELF table entries must be trivially copyable");

  public:
    using Decoder = Entry (*)(u8 const *data) noexcept;

    class Iterator final {
      public:
        using value_type = Entry;

        [[nodiscard]]
        auto operator*() const noexcept -> Entry {
            return m_decoder(m_data + m_index * sizeof(Entry));
        }

        auto operator++() noexcept -> Iterator & {
            ++m_index;
            return *this;
        }

        [[nodiscard]]
        constexpr auto operator==(Iterator const &) const noexcept -> bool = default;

      private:
        friend class TableView;

        constexpr Iterator(u8 const *data, usize const index, Decoder const decoder) noexcept
            : m_data(data), m_index(index), m_decoder(decoder) {}

        u8 const *m_data{nullptr};
        usize m_index{0};
        Decoder m_decoder{&decode_copy};
    };

    constexpr TableView() noexcept = default;

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_count == 0;
    }

    [[nodiscard]]
    static auto from_checked_bytes(ByteRange const &bytes, usize const count,
                                   Decoder const decoder = &decode_copy) noexcept
        -> ustl::Option<TableView> {
        if (count > std::numeric_limits<usize>::max() / sizeof(Entry) ||
            bytes.size() != count * sizeof(Entry) || decoder == nullptr ||
            (!bytes.empty() && bytes.data() == nullptr)) {
            return ustl::NONE;
        }
        return ustl::some(TableView(bytes.data(), count, decoder));
    }

    [[nodiscard]]
    auto at(usize const index) const noexcept -> ustl::Option<Entry> {
        if (index >= m_count) {
            return ustl::NONE;
        }
        return ustl::some(read(index));
    }

    [[nodiscard]]
    auto operator[](usize const index) const noexcept -> Entry {
        return at(index).value();
    }

    [[nodiscard]]
    auto first(usize const count) const noexcept -> ustl::Option<TableView> {
        if (count > m_count) {
            return ustl::NONE;
        }
        return ustl::some(TableView(m_data, count, m_decoder));
    }

    [[nodiscard]]
    constexpr auto begin() const noexcept -> Iterator {
        return Iterator(m_data, 0, m_decoder);
    }

    [[nodiscard]]
    constexpr auto end() const noexcept -> Iterator {
        return Iterator(m_data, m_count, m_decoder);
    }

  private:
    [[nodiscard]]
    static auto decode_copy(u8 const *data) noexcept -> Entry {
        Entry entry{};
        std::memcpy(&entry, data, sizeof(entry));
        return entry;
    }

    constexpr TableView(u8 const *data, usize const count, Decoder const decoder) noexcept
        : m_data(data), m_count(count), m_decoder(decoder) {}

    [[nodiscard]]
    auto read(usize const index) const noexcept -> Entry {
        return m_decoder(m_data + index * sizeof(Entry));
    }

    u8 const *m_data{nullptr};
    usize m_count{0};
    Decoder m_decoder{&decode_copy};
};
} // namespace eftl::elf

#endif // EFTL_ELF_TABLE_HPP
