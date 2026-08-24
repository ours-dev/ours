/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef MULTIBOOT_VIEW_HPP
#define MULTIBOOT_VIEW_HPP 1

#include <multiboot/multiboot2.hpp>
#include <ours/status.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <limits>

#include <cstdint>
#include <cstring>

namespace multiboot {
using ours::Status;

/// A validated, allocation-free view of the Multiboot2 information block
/// that supplies the OBI module to physboot.
class View final {
  public:
    using Bytes = ustl::views::Span<u8 const>;
    using Result = ustl::Result<View, Status>;

    struct Module final {
        u32 start;
        u32 end;
    };
    using ModuleResult = ustl::Result<Module, Status>;

    class Tag final {
      public:
        [[nodiscard]]
        auto type() const -> u32 {
            return type_;
        }

        [[nodiscard]]
        auto bytes() const -> Bytes {
            return bytes_;
        }

      private:
        friend class View;
        friend class Iterator;

        Tag(u32 const type, Bytes const bytes) : type_(type), bytes_(bytes) {}

        u32 type_;
        Bytes bytes_;
    };

    class MemoryMap final {
      public:
        struct Range final {
            u64 addr;
            u64 len;
            u32 type;
        };

        using Result = ustl::Result<MemoryMap, Status>;

        [[nodiscard]]
        static auto from(Tag const &tag) -> Result {
            auto const bytes = tag.bytes();
            if (tag.type() != MULTIBOOT_TAG_TYPE_MMAP || bytes.size() < sizeof(MultibootTagMmap)) {
                return ustl::err(Status::InvalidArgument);
            }

            MultibootTagMmap header{};
            std::memcpy(&header, bytes.data(), sizeof(header));
            if (header.entry_size < sizeof(MultibootMemoryMap) ||
                (bytes.size() - sizeof(header)) % header.entry_size != 0) {
                return ustl::err(Status::InvalidArgument);
            }
            if (header.entry_version != 0) {
                return ustl::err(Status::NotSupported);
            }

            for (usize offset = sizeof(header); offset < bytes.size();
                 offset += header.entry_size) {
                MultibootMemoryMap entry{};
                std::memcpy(&entry, bytes.data() + offset, sizeof(entry));
                if (entry.zero != 0 ||
                    (entry.len != 0 && entry.addr > std::numeric_limits<u64>::max() - entry.len)) {
                    return ustl::err(Status::InvalidArgument);
                }
            }
            return ustl::ok(MemoryMap(bytes, header.entry_size));
        }

        [[nodiscard]]
        auto count() const -> usize {
            return (bytes_.size() - sizeof(MultibootTagMmap)) / entry_size_;
        }

        [[nodiscard]]
        auto range(usize const index) const -> Range {
            MultibootMemoryMap entry{};
            std::memcpy(&entry, bytes_.data() + sizeof(MultibootTagMmap) + index * entry_size_,
                        sizeof(entry));
            return {
                .addr = entry.addr,
                .len = entry.len,
                .type = entry.type,
            };
        }

      private:
        MemoryMap(Bytes const bytes, u32 const entry_size)
            : bytes_(bytes), entry_size_(entry_size) {}

        Bytes bytes_;
        u32 entry_size_;
    };

    using MemoryMapResult = ustl::Result<ustl::Option<MemoryMap>, Status>;

    class Iterator final {
      public:
        [[nodiscard]]
        auto next() -> ustl::Option<Tag> {
            if (cursor_ == size_) {
                return ustl::NONE;
            }

            MultibootTag header{};
            std::memcpy(&header, data_ + cursor_, sizeof(header));
            auto const aligned_size = align_up(header.size);
            auto const next_cursor = cursor_ + aligned_size;
            if (header.type == MULTIBOOT_TAG_TYPE_END) {
                cursor_ = size_;
                return ustl::NONE;
            }

            auto const tag = Tag(header.type, Bytes(data_ + cursor_, header.size));
            cursor_ = next_cursor;
            return ustl::Option<Tag>(tag);
        }

      private:
        friend class View;

        Iterator(u8 const *data, usize const size)
            : data_(data), size_(size), cursor_(sizeof(InfoHeader)) {}

        u8 const *data_;
        usize size_;
        usize cursor_;
    };

    [[nodiscard]]
    static auto from(u8 const *data, usize const size) -> Result {
        if (data == nullptr || size < sizeof(InfoHeader) ||
            reinterpret_cast<std::uintptr_t>(data) % MULTIBOOT_INFO_ALIGN != 0) {
            return ustl::err(Status::InvalidArgument);
        }

        InfoHeader header{};
        std::memcpy(&header, data, sizeof(header));
        if (header.total_size != size || header.reserved != 0 || size % MULTIBOOT_INFO_ALIGN != 0) {
            return ustl::err(Status::InvalidArgument);
        }

        auto cursor = usize{sizeof(InfoHeader)};
        while (cursor < size) {
            if (size - cursor < sizeof(MultibootTag)) {
                return ustl::err(Status::InvalidArgument);
            }

            MultibootTag tag{};
            std::memcpy(&tag, data + cursor, sizeof(tag));
            if (tag.size < sizeof(MultibootTag)) {
                return ustl::err(Status::InvalidArgument);
            }
            auto const aligned_size = align_up(tag.size);
            if (aligned_size > size - cursor) {
                return ustl::err(Status::InvalidArgument);
            }

            cursor += aligned_size;
            if (tag.type != MULTIBOOT_TAG_TYPE_END) {
                continue;
            }
            if (tag.size != sizeof(MultibootTag) || cursor != size) {
                return ustl::err(Status::InvalidArgument);
            }
            return ustl::ok(View(data, size));
        }
        return ustl::err(Status::InvalidArgument);
    }

    [[nodiscard]]
    auto iterator() const -> Iterator {
        return Iterator(data_, size_);
    }

    [[nodiscard]]
    auto module() const -> ModuleResult {
        auto result = ustl::Option<Module>{ustl::NONE};
        auto tags = iterator();
        while (auto const tag = tags.next()) {
            if (tag->type() != MULTIBOOT_TAG_TYPE_MODULE) {
                continue;
            }
            if (tag->bytes().size() < sizeof(MultibootTagModule)) {
                return ustl::err(Status::InvalidArgument);
            }

            MultibootTagModule module{};
            std::memcpy(&module, tag->bytes().data(), sizeof(module));
            if (module.mod_start >= module.mod_end || module.mod_start % MULTIBOOT_MOD_ALIGN != 0) {
                return ustl::err(Status::InvalidArgument);
            }
            if (result.is_some()) {
                return ustl::err(Status::AlreadyExists);
            }
            result = ustl::Option<Module>(Module{
                .start = module.mod_start,
                .end = module.mod_end,
            });
        }
        if (result.is_none()) {
            return ustl::err(Status::NotFound);
        }
        return ustl::ok(result.value());
    }

    [[nodiscard]]
    auto memory_map() const -> MemoryMapResult {
        auto selected = ustl::Option<Tag>{ustl::NONE};
        auto tags = iterator();
        while (auto const tag = tags.next()) {
            if (tag->type() != MULTIBOOT_TAG_TYPE_MMAP) {
                continue;
            }
            if (selected.is_some()) {
                return ustl::err(Status::AlreadyExists);
            }
            selected = ustl::Option<Tag>(*tag);
        }
        if (selected.is_none()) {
            return ustl::ok(ustl::Option<MemoryMap>{ustl::NONE});
        }
        auto const parsed = MemoryMap::from(selected.value());
        if (parsed.is_err()) {
            return ustl::err(parsed.unwrap_err());
        }
        return ustl::ok(ustl::Option<MemoryMap>(parsed.unwrap()));
    }

  private:
    struct InfoHeader final {
        u32 total_size;
        u32 reserved;
    };

    [[nodiscard]]
    static auto align_up(u32 const size) -> usize {
        return (static_cast<usize>(size) + MULTIBOOT_TAG_ALIGN - 1) &
               ~static_cast<usize>(MULTIBOOT_TAG_ALIGN - 1);
    }

    View(u8 const *data, usize const size) : data_(data), size_(size) {}

    u8 const *data_;
    usize size_;
};
} // namespace multiboot

#endif // MULTIBOOT_VIEW_HPP
