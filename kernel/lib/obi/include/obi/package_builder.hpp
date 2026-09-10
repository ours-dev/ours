/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_PACKAGE_BUILDER_HPP
#define OBI_PACKAGE_BUILDER_HPP 1

#include <ours/status.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

#include <cstring>

#include <obi/checksum.hpp>
#include <obi/compression.hpp>
#include <obi/package_format.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace obi {

struct ItemSource final {
    std::string name;
    ItemType type;
    ItemFormat format;
    ustl::views::Span<u8 const> data;
    ItemFlags flags = ItemFlags::Required;
    u64 memory_size = 0; // defaults to data.size()
    u64 alignment = ContainerHeader::ALIGNMENT;
    Compression compression = Compression::None;
};

class PackageBuilder final {
  public:
    using Result = ustl::Result<void, ours::Status>;

    PackageBuilder() = default;

    auto add_item(ItemSource const &source) -> Result {
        if (source.name.empty() || source.name.size() > 0xFFFF) {
            return ustl::err(ours::Status::InvArgs);
        }
        if ((static_cast<u32>(source.flags) & ~SUPPORTED_ITEM_FLAGS) != 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        if (source.compression != Compression::None && source.compression != Compression::Lz4) {
            return ustl::err(ours::Status::NotSupported);
        }
        if (sources_.size() >= ContainerHeader::MAX_ITEMS) {
            return ustl::err(ours::Status::NoSpace);
        }
        if (source.alignment == 0 || (source.alignment & (source.alignment - 1)) != 0) {
            return ustl::err(ours::Status::InvArgs);
        }
        // Reject duplicate names.
        for (auto const &existing: sources_) {
            if (existing.name == source.name) {
                return ustl::err(ours::Status::AlreadyExists);
            }
        }
        sources_.push_back(source);
        return ustl::ok();
    }

    auto build(bool crc = false) -> std::vector<u8> {
        if (sources_.empty()) {
            return {};
        }

        // Executable (kernel) items must come first — the bootloader
        // requires the first item to be the entry point.
        std::stable_sort(sources_.begin(), sources_.end(),
                         [](ItemSource const &a, ItemSource const &b) {
                             bool a_is_kernel = (a.type == ItemType::Executable);
                             bool b_is_kernel = (b.type == ItemType::Executable);
                             return a_is_kernel && !b_is_kernel;
                         });

        auto const item_count = static_cast<u32>(sources_.size());
        auto const item_table_offset = static_cast<u64>(sizeof(ContainerHeader));
        auto const string_table_size = compute_string_table_size();

        auto const item_table_end =
            item_table_offset + static_cast<u64>(item_count) * sizeof(ItemDescriptor);
        auto const string_table_offset = item_table_end;

        // Layout payloads: each starts at an aligned offset.
        auto cursor = string_table_offset + static_cast<u64>(string_table_size);
        cursor = align_up(cursor, ContainerHeader::ALIGNMENT);

        struct PayloadLayout {
            u64 offset;
            u64 size;
        };
        std::vector<PayloadLayout> layouts;
        layouts.reserve(sources_.size());

        for (auto const &source: sources_) {
            auto const align = std::max(ContainerHeader::ALIGNMENT, source.alignment);
            cursor = align_up(cursor, align);
            layouts.push_back({cursor, static_cast<u64>(source.data.size())});
            cursor += static_cast<u64>(source.data.size());
        }

        // The container's overall size must itself be a multiple of the
        // alignment (ContainerView::from() requires this), even though
        // the last payload's own end may not land on that boundary.
        auto const container_size = align_up(cursor, ContainerHeader::ALIGNMENT);

        std::vector<u8> container(static_cast<std::size_t>(container_size), u8{0});

        // Header
        ContainerHeader header{};
        header.magic = ContainerHeader::MAGIC;
        header.major = ContainerHeader::MAJOR;
        header.minor = ContainerHeader::MINOR;
        header.header_size = sizeof(ContainerHeader);
        header.container_size = container_size;
        header.item_table_offset = item_table_offset;
        header.item_count = item_count;
        header.string_table_offset = static_cast<u32>(string_table_offset);
        header.string_table_size = static_cast<u32>(string_table_size);
        header.setup_item_index = find_setup_index();
        header.flags = crc ? static_cast<u32>(ContainerFlags::Crc32) : 0;
        std::memcpy(container.data(), &header, sizeof(header));

        // String table
        auto *string_table = container.data() + string_table_offset;
        for (auto const &source: sources_) {
            std::memcpy(string_table, source.name.data(), source.name.size());
            string_table += source.name.size();
        }

        // Payloads — written before the item descriptors so that, when
        // CRC is requested, each item's digest can be computed over the
        // real payload bytes rather than the buffer's still-zeroed
        // initial contents.
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            std::memcpy(container.data() + layouts[i].offset, sources_[i].data.data(),
                        sources_[i].data.size());
        }

        // Item descriptors
        auto *const descriptors =
            reinterpret_cast<ItemDescriptor *>(container.data() + item_table_offset);
        auto name_offset = u32{0};
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            auto const &source = sources_[i];
            auto const &layout = layouts[i];

            auto memory_size = source.memory_size;
            if (memory_size == 0) {
                memory_size = source.data.size();
            }

            ItemDescriptor desc{};
            desc.name_offset = name_offset;
            desc.name_size = static_cast<u16>(source.name.size());
            desc.type = static_cast<u32>(source.type);
            desc.flags = static_cast<u32>(source.flags);
            desc.format = static_cast<u32>(source.format);
            desc.compression = static_cast<u32>(source.compression);
            desc.payload_offset = layout.offset;
            desc.payload_size = layout.size;
            desc.memory_size = memory_size;
            desc.alignment = source.alignment;

            if (crc) {
                auto const digest =
                    crc32(container.data() + layout.offset, static_cast<usize>(layout.size));
                std::memcpy(desc.digest, &digest, sizeof(digest));
            }

            std::memcpy(&descriptors[i], &desc, sizeof(desc));
            name_offset += desc.name_size;
        }

        // Container CRC
        if (crc) {
            auto const container_digest =
                crc32(container.data(), static_cast<usize>(container_size),
                      offsetof(ContainerHeader, container_digest), sizeof(header.container_digest));
            std::memcpy(container.data() + offsetof(ContainerHeader, container_digest),
                        &container_digest, sizeof(container_digest));
        }

        return container;
    }

  private:
    [[nodiscard]]
    auto find_setup_index() const -> u32 {
        for (u32 i = 0; i < sources_.size(); ++i) {
            auto const &s = sources_[i];
            if (s.name == "setup" && s.type == ItemType::Config && s.format == ItemFormat::Yaml) {
                return i;
            }
        }
        return ContainerHeader::NO_SETUP_ITEM;
    }

    [[nodiscard]]
    auto compute_string_table_size() const -> usize {
        usize size = 0;
        for (auto const &source: sources_) {
            size += source.name.size();
        }
        return size;
    }

    static auto align_up(u64 value, u64 alignment) -> u64 {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::vector<ItemSource> sources_;
};

} // namespace obi

#endif
