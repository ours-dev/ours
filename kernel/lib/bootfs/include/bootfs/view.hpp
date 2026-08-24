/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTFS_VIEW_HPP
#define BOOTFS_VIEW_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <limits>
#include <string_view>

#include <cstring>

#include <bootfs/format.hpp>

namespace bootfs {
class View final {
  public:
    using Bytes = ustl::views::Span<u8 const>;
    using Result = ustl::Result<View, ours::Status>;
    using EntryResult = ustl::Result<Entry, ours::Status>;
    using FindResult = ustl::Result<ustl::Option<Entry>, ours::Status>;

    /// Parse and validate a BootFS blob.
    [[nodiscard]]
    static auto from(Bytes data) -> Result {
        if (data.data() == nullptr || data.size() < sizeof(Header)) {
            return ustl::err(ours::Status::InvalidArgument);
        }

        Header header{};
        std::memcpy(&header, data.data(), sizeof(header));
        if (header.magic != Header::kMagic || header.reserved != 0 || header.reserved2 != 0 ||
            header.file_count == 0) {
            return ustl::err(ours::Status::InvalidArgument);
        }

        auto const entry_table_size = static_cast<u64>(header.file_count) * sizeof(Entry);
        auto const entry_table_end = checked_add(sizeof(Header), entry_table_size);
        if (entry_table_end.is_none() || entry_table_end.value() > data.size()) {
            return ustl::err(ours::Status::OutOfRange);
        }

        auto const name_table_offset = entry_table_end.value();
        auto const name_table_end = checked_add(name_table_offset, header.name_table_size);
        if (name_table_end.is_none() || name_table_end.value() > data.size()) {
            return ustl::err(ours::Status::OutOfRange);
        }

        if (header.total_size != data.size()) {
            return ustl::err(ours::Status::InvalidArgument);
        }

        auto const *entries = reinterpret_cast<Entry const *>(data.data() + sizeof(Header));
        auto const *name_table = data.data() + name_table_offset;

        for (u32 index = 0; index < header.file_count; ++index) {
            Entry entry{};
            std::memcpy(&entry, entries + index, sizeof(entry));
            if (entry.name_size == 0 || entry.flags != 0 || !is_power_of_two(entry.alignment) ||
                entry.name_offset > header.name_table_size ||
                static_cast<u64>(entry.name_offset) + entry.name_size > header.name_table_size ||
                std::memchr(name_table + entry.name_offset, 0, entry.name_size) != nullptr ||
                entry.data_offset < name_table_end.value() ||
                entry.data_offset % entry.alignment != 0) {
                return ustl::err(ours::Status::InvalidArgument);
            }
            auto const data_end = checked_add(entry.data_offset, entry.data_size);
            if (data_end.is_none() || data_end.value() > header.total_size) {
                return ustl::err(ours::Status::OutOfRange);
            }
        }

        return ustl::ok(View(data.data(), header, entries, name_table));
    }

    [[nodiscard]]
    auto file_count() const -> u32 {
        return header_.file_count;
    }

    [[nodiscard]]
    auto total_size() const -> u64 {
        return header_.total_size;
    }

    [[nodiscard]]
    auto file(u32 index) const -> EntryResult {
        if (index >= file_count()) {
            return ustl::err(ours::Status::OutOfRange);
        }
        Entry entry{};
        std::memcpy(&entry, entries_ + index, sizeof(entry));
        return ustl::ok(entry);
    }

    [[nodiscard]]
    auto find(std::string_view name) const -> FindResult {
        auto match = ustl::Option<Entry>{ustl::NONE};
        for (u32 index = 0; index < file_count(); ++index) {
            auto const candidate = file(index);
            if (candidate.is_err()) {
                return ustl::err(candidate.unwrap_err());
            }
            auto const candidate_name = file_name(candidate.unwrap());
            if (candidate_name != name) {
                continue;
            }
            if (match.is_some()) {
                return ustl::err(ours::Status::AlreadyExists);
            }
            match = ustl::Option<Entry>(candidate.unwrap());
        }
        return ustl::ok(match);
    }

    [[nodiscard]]
    auto file_name(Entry const &entry) const -> std::string_view {
        return {
            reinterpret_cast<char const *>(name_table_ + entry.name_offset),
            entry.name_size,
        };
    }

    [[nodiscard]]
    auto file_name(u32 index) const -> std::string_view {
        Entry entry{};
        std::memcpy(&entry, entries_ + index, sizeof(entry));
        return file_name(entry);
    }

    [[nodiscard]]
    auto file_data(Entry const &entry) const -> Bytes {
        return {
            data_ + entry.data_offset,
            static_cast<usize>(entry.data_size),
        };
    }

    [[nodiscard]]
    auto file_data(u32 index) const -> Bytes {
        Entry entry{};
        std::memcpy(&entry, entries_ + index, sizeof(entry));
        return file_data(entry);
    }

  private:
    [[nodiscard]]
    static auto checked_add(u64 left, u64 right) -> ustl::Option<u64> {
        if (right > std::numeric_limits<u64>::max() - left) {
            return ustl::NONE;
        }
        return ustl::Option<u64>(left + right);
    }

    [[nodiscard]]
    static auto is_power_of_two(u64 value) -> bool {
        return value != 0 && (value & (value - 1)) == 0;
    }

    View(u8 const *data, Header header, Entry const *entries, u8 const *name_table)
        : data_(data), header_(header), entries_(entries), name_table_(name_table) {}

    u8 const *data_;
    Header header_;
    Entry const *entries_;
    u8 const *name_table_;
};
} // namespace bootfs

#endif // BOOTFS_VIEW_HPP
