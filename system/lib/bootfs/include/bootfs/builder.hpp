/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTFS_BUILDER_HPP
#define BOOTFS_BUILDER_HPP 1

#include <bootfs/format.hpp>

#include <string>
#include <vector>

#include <cstring>

#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace bootfs {
/// Where every file's data starts, and what the BootFS is padded out to: a
/// page, as Zircon's BootFS format requires (ZBI_BOOTFS_PAGE_SIZE in
/// lib/zbi-format/internal/bootfs.h). A program is started by slicing its
/// file out of the BootFS VMO (vmo_create_child, launch::launch_from_bootfs()),
/// and a slice begins on a page; at 8 bytes, the alignment this had before,
/// no file of a BootFS userboot unpacked could be sliced. The BootFS the
/// kernel used to lay out itself aligned its files to a page for the same
/// reason.
inline constexpr auto ALIGNMENT = u64{4096};

struct File final {
    std::string name;
    ustl::views::Span<u8 const> data;
};

class Builder final {
  public:
    auto add_file(std::string name, ustl::views::Span<u8 const> data) -> void {
        files_.push_back({std::move(name), data});
    }

    [[nodiscard]]
    auto file_count() const -> std::size_t {
        return files_.size();
    }

    auto build() -> std::vector<u8> {
        if (files_.empty()) {
            return {};
        }

        auto const count = static_cast<u32>(files_.size());
        auto const entry_off = static_cast<u64>(sizeof(Header));
        auto const name_sz = compute_name_table_size();
        auto const name_off = entry_off + static_cast<u64>(count) * sizeof(Entry);

        auto cursor = name_off + name_sz;
        cursor = align_up(cursor);

        struct Layout {
            u64 off;
        };
        std::vector<Layout> layouts;
        layouts.reserve(files_.size());
        for (auto const &f: files_) {
            layouts.push_back({cursor});
            cursor += static_cast<u64>(f.data.size());
            cursor = align_up(cursor);
        }

        auto const total = cursor;
        std::vector<u8> blob(static_cast<std::size_t>(total), u8{0});

        Header hdr{};
        hdr.magic = Header::MAGIC;
        hdr.file_count = count;
        hdr.name_table_size = static_cast<u32>(name_sz);
        hdr.total_size = total;
        std::memcpy(blob.data(), &hdr, sizeof(hdr));

        auto *entries = reinterpret_cast<Entry *>(blob.data() + entry_off);
        auto name_cursor = u32{0};
        for (std::size_t i = 0; i < files_.size(); ++i) {
            Entry e{};
            e.name_offset = name_cursor;
            e.name_size = static_cast<u16>(files_[i].name.size());
            e.data_offset = layouts[i].off;
            e.data_size = static_cast<u64>(files_[i].data.size());
            e.alignment = ALIGNMENT;
            std::memcpy(&entries[i], &e, sizeof(e));
            name_cursor += e.name_size;
        }

        auto *names = blob.data() + name_off;
        for (auto const &f: files_) {
            std::memcpy(names, f.name.data(), f.name.size());
            names += f.name.size();
        }

        for (std::size_t i = 0; i < files_.size(); ++i) {
            std::memcpy(blob.data() + layouts[i].off, files_[i].data.data(), files_[i].data.size());
        }

        return blob;
    }

  private:
    [[nodiscard]]
    auto compute_name_table_size() const -> u64 {
        u64 sz = 0;
        for (auto const &f: files_) {
            sz += f.name.size();
        }
        return sz;
    }

    static auto align_up(u64 v, u64 a = ALIGNMENT) -> u64 {
        return (v + a - 1) & ~(a - 1);
    }

    std::vector<File> files_;
};
} // namespace bootfs

#endif // BOOTFS_BUILDER_HPP
