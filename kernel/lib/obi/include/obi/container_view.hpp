/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_CONTAINER_VIEW_HPP
#define OBI_CONTAINER_VIEW_HPP 1

#include <ours/status.hpp>

#include <limits>
#include <span>
#include <string_view>

#include <cstring>

#include <obi/checksum.hpp>
#include <obi/compression.hpp>
#include <obi/package_format.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace obi {
class ContainerView;

class ItemView final {
  public:
    using Bytes = std::span<u8 const>;
    using MutableBytes = std::span<u8>;
    using MaterializeResult = ustl::Result<usize, ours::Status>;

    [[nodiscard]]
    auto name() const -> std::string_view {
        return {reinterpret_cast<char const *>(string_table_ + descriptor_.name_offset),
                descriptor_.name_size};
    }

    [[nodiscard]]
    auto type() const -> ItemType {
        return static_cast<ItemType>(descriptor_.type);
    }

    [[nodiscard]]
    auto format() const -> ItemFormat {
        return static_cast<ItemFormat>(descriptor_.format);
    }

    [[nodiscard]]
    auto compression() const -> Compression {
        return static_cast<Compression>(descriptor_.compression);
    }

    [[nodiscard]]
    auto flags() const -> ItemFlags {
        return static_cast<ItemFlags>(descriptor_.flags);
    }

    [[nodiscard]]
    auto payload() const -> Bytes {
        return {data_ + descriptor_.payload_offset, static_cast<usize>(descriptor_.payload_size)};
    }

    [[nodiscard]]
    auto payload_size() const -> u64 {
        return descriptor_.payload_size;
    }

    /// Copy or decompress the item into its declared in-memory
    /// representation.  The output may be larger than the stored payload;
    /// the tail is always zero-filled.
    [[nodiscard]]
    auto materialize(MutableBytes output) const -> MaterializeResult {
        if (output.data() == nullptr && output.size() != 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        auto const size = static_cast<usize>(descriptor_.memory_size);
        if (output.size() < size) {
            return ustl::err(ours::Status::NoSpace);
        }
        auto const stored =
            Bytes(data_ + descriptor_.payload_offset, static_cast<usize>(descriptor_.payload_size));
        usize materialized_size = 0;
        switch (compression()) {
            case Compression::None:
                if (descriptor_.payload_size != 0) {
                    std::memcpy(output.data(), stored.data(), stored.size());
                }
                materialized_size = stored.size();
                break;
            case Compression::Lz4: {
                auto const decoded =
                    compression::decompress(stored, MutableBytes(output.data(), size));
                if (decoded.is_err()) {
                    return ustl::err(decoded.unwrap_err());
                }
                materialized_size = decoded.unwrap();
                break;
            }
            default:
                return ustl::err(ours::Status::NotSupported);
        }
        if (size > materialized_size) {
            std::memset(output.data() + materialized_size, 0, size - materialized_size);
        }
        return ustl::ok(size);
    }

    [[nodiscard]]
    auto memory_size() const -> u64 {
        return descriptor_.memory_size;
    }

    [[nodiscard]]
    auto alignment() const -> u64 {
        return descriptor_.alignment;
    }

  private:
    friend class ContainerView;

    ItemView(u8 const *data, u8 const *string_table, ItemDescriptor descriptor)
        : data_(data), string_table_(string_table), descriptor_(descriptor) {}

    u8 const *data_;
    u8 const *string_table_;
    ItemDescriptor descriptor_;
};

class ContainerView final {
  public:
    using Result = ustl::Result<ContainerView, ours::Status>;
    using ItemResult = ustl::Result<ItemView, ours::Status>;

    [[nodiscard]]
    static auto from(u8 const *data, usize size) -> Result {
        if (data == nullptr || size < sizeof(ContainerHeader)) {
            return ustl::err(ours::Status::InvArgs);
        }

        ContainerHeader header{};
        std::memcpy(&header, data, sizeof(header));
        if (header.magic != ContainerHeader::MAGIC || header.major != ContainerHeader::MAJOR ||
            header.header_size != sizeof(ContainerHeader) || header.reserved0 != 0 ||
            header.reserved != 0 ||
            (header.flags & ~static_cast<u32>(ContainerFlags::Crc32)) != 0 ||
            header.item_count > ContainerHeader::MAX_ITEMS ||
            header.container_size < header.header_size || header.container_size != size ||
            header.container_size % ContainerHeader::ALIGNMENT != 0 ||
            header.container_size > std::numeric_limits<usize>::max()) {
            return ustl::err(ours::Status::InvArgs);
        }
        if (header.minor > ContainerHeader::MINOR) {
            return ustl::err(ours::Status::NotSupported);
        }

        auto const container_size = static_cast<usize>(header.container_size);
        auto const item_table_offset = header.item_table_offset;
        auto const item_table_size = static_cast<u64>(header.item_count) * sizeof(ItemDescriptor);
        auto const item_table_end = checked_add(item_table_offset, item_table_size);
        if (item_table_end.is_none() || item_table_offset != header.header_size ||
            item_table_offset % ContainerHeader::ALIGNMENT != 0 ||
            item_table_end.value() > header.container_size) {
            return ustl::err(ours::Status::OutOfRange);
        }

        auto const string_table_offset = static_cast<u64>(header.string_table_offset);
        auto const string_table_end = checked_add(string_table_offset, header.string_table_size);
        if (string_table_end.is_none() || string_table_offset != item_table_end.value() ||
            string_table_end.value() > header.container_size) {
            return ustl::err(ours::Status::OutOfRange);
        }

        if (header.setup_item_index != ContainerHeader::NO_SETUP_ITEM &&
            header.setup_item_index >= header.item_count) {
            return ustl::err(ours::Status::OutOfRange);
        }

        auto const *items = data + static_cast<usize>(item_table_offset);
        // Descriptor-order payloads make overlap validation a streaming check.
        auto minimum_payload_offset = string_table_end.value();
        for (u32 index = 0; index < header.item_count; ++index) {
            ItemDescriptor item{};
            std::memcpy(&item, items + index * sizeof(ItemDescriptor), sizeof(item));
            if (item.reserved_name != 0 || item.reserved != 0 || item.name_size == 0 ||
                (item.flags & ~SUPPORTED_ITEM_FLAGS) != 0 ||
                item.name_size > header.string_table_size ||
                static_cast<u64>(item.name_offset) + item.name_size > header.string_table_size ||
                item.payload_offset < string_table_end.value() ||
                item.payload_offset < minimum_payload_offset ||
                item.payload_offset % ContainerHeader::ALIGNMENT != 0 ||
                !is_power_of_two(item.alignment) || item.payload_offset % item.alignment != 0 ||
                item.memory_size > std::numeric_limits<usize>::max()) {
                return ustl::err(ours::Status::InvArgs);
            }
            if (item.compression == static_cast<u32>(Compression::None) &&
                item.memory_size < item.payload_size) {
                return ustl::err(ours::Status::InvArgs);
            }
            if (std::memchr(data + string_table_offset + item.name_offset, 0, item.name_size) !=
                nullptr) {
                return ustl::err(ours::Status::InvArgs);
            }

            if (item.compression != static_cast<u32>(Compression::None) &&
                item.compression != static_cast<u32>(Compression::Lz4)) {
                return ustl::err(ours::Status::NotSupported);
            }

            auto const payload_end = checked_add(item.payload_offset, item.payload_size);
            if (payload_end.is_none() || payload_end.value() > header.container_size) {
                return ustl::err(ours::Status::OutOfRange);
            }
            if (item.compression == static_cast<u32>(Compression::Lz4)) {
                auto const decoded_size = compression::decompressed_size(
                    ItemView::Bytes(data + static_cast<usize>(item.payload_offset),
                                    static_cast<usize>(item.payload_size)));
                if (decoded_size.is_err()) {
                    return ustl::err(decoded_size.unwrap_err());
                }
                if (decoded_size.unwrap() > item.memory_size) {
                    return ustl::err(ours::Status::InvArgs);
                }
            }

            if ((header.flags & static_cast<u32>(ContainerFlags::Crc32)) != 0) {
                if (!digest_tail_is_zero(item.digest) ||
                    read_digest(item.digest) !=
                        crc32(data + item.payload_offset, static_cast<usize>(item.payload_size))) {
                    return ustl::err(ours::Status::InvArgs);
                }
            } else if (!digest_is_zero(item.digest)) {
                return ustl::err(ours::Status::InvArgs);
            }
            minimum_payload_offset = payload_end.value();
        }

        if ((header.flags & static_cast<u32>(ContainerFlags::Crc32)) != 0) {
            if (!digest_tail_is_zero(header.container_digest) ||
                read_digest(header.container_digest) !=
                    crc32(data, container_size, offsetof(ContainerHeader, container_digest),
                          sizeof(header.container_digest))) {
                return ustl::err(ours::Status::InvArgs);
            }
        } else if (!digest_is_zero(header.container_digest)) {
            return ustl::err(ours::Status::InvArgs);
        }

        return ustl::ok(ContainerView(data, header, data + static_cast<usize>(string_table_offset),
                                      data + static_cast<usize>(item_table_offset)));
    }

    [[nodiscard]]
    auto item_count() const -> usize {
        return header_.item_count;
    }

    [[nodiscard]]
    auto container_size() const -> usize {
        return static_cast<usize>(header_.container_size);
    }

    [[nodiscard]]
    auto bytes() const -> ItemView::Bytes {
        return {data_, container_size()};
    }

    [[nodiscard]]
    auto setup_item_index() const -> u32 {
        return header_.setup_item_index;
    }

    [[nodiscard]]
    auto flags() const -> ContainerFlags {
        return static_cast<ContainerFlags>(header_.flags);
    }

    [[nodiscard]]
    auto item(usize index) const -> ItemResult {
        if (index >= item_count()) {
            return ustl::err(ours::Status::OutOfRange);
        }

        ItemDescriptor descriptor{};
        std::memcpy(&descriptor, item_table_ + index * sizeof(ItemDescriptor), sizeof(descriptor));
        return ustl::ok(ItemView(data_, string_table_, descriptor));
    }

    using FindResult = ustl::Result<ustl::Option<ItemView>, ours::Status>;

    /// Look up a single item by name.  A name that matches more than one
    /// item is ambiguous and reported as `AlreadyExists` rather than
    /// silently returning the first match.
    [[nodiscard]]
    auto find(std::string_view name) const -> FindResult {
        ustl::Option<ItemView> match{ustl::NONE};
        for (usize index = 0; index < item_count(); ++index) {
            auto candidate = item(index);
            if (candidate.is_err()) {
                return ustl::err(candidate.unwrap_err());
            }
            if (candidate.unwrap().name() != name) {
                continue;
            }
            if (match.is_some()) {
                return ustl::err(ours::Status::AlreadyExists);
            }
            match = ustl::Option<ItemView>(candidate.unwrap());
        }
        return ustl::ok(match);
    }

  private:
    [[nodiscard]]
    static auto read_digest(u8 const *digest) -> u32 {
        u32 value{};
        std::memcpy(&value, digest, sizeof(value));
        return value;
    }

    [[nodiscard]]
    static auto digest_tail_is_zero(u8 const *digest) -> bool {
        for (usize index = sizeof(u32); index < 32; ++index) {
            if (digest[index] != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    static auto digest_is_zero(u8 const *digest) -> bool {
        for (usize index = 0; index < 32; ++index) {
            if (digest[index] != 0) {
                return false;
            }
        }
        return true;
    }

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

    ContainerView(u8 const *data, ContainerHeader header, u8 const *string_table,
                  u8 const *item_table)
        : data_(data), header_(header), string_table_(string_table), item_table_(item_table) {}

    u8 const *data_;
    ContainerHeader header_;
    u8 const *string_table_;
    u8 const *item_table_;
};
} // namespace obi

#endif // OBI_CONTAINER_VIEW_HPP
