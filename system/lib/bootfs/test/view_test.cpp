/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// bootfs::View::from() against the page rule, stated at bootfs::ALIGNMENT in
/// <bootfs/format.hpp>, and a Builder round trip. Each refusal starts from a
/// BootFS the Builder laid out, which keeps the rule, and edits one entry to
/// break it, so that nothing else about the image is wrong.
#include <bootfs/builder.hpp>
#include <bootfs/format.hpp>
#include <bootfs/view.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>

namespace {
using bootfs::ALIGNMENT;
using bootfs::Builder;
using bootfs::Entry;
using bootfs::View;
using ours::Status;

/// |size| bytes counting up from |seed|, so that two files made with
/// different seeds never hold the same bytes.
auto bytes_of(usize const size, u8 const seed) -> std::vector<u8> {
    auto bytes = std::vector<u8>(size);
    auto value = seed;
    for (auto &byte: bytes) {
        byte = value;
        ++value;
    }
    return bytes;
}

/// The files of one BootFS, kept alive while the Builder lays them out:
/// Builder::add_file() holds only a view of each file's bytes.
struct TestFile final {
    std::string name;
    std::vector<u8> data;
};

auto build(std::vector<TestFile> const &files) -> std::vector<u8> {
    auto builder = Builder{};
    for (auto const &file: files) {
        builder.add_file(file.name, {file.data.data(), file.data.size()});
    }
    return builder.build();
}

auto entry_of(std::vector<u8> const &blob, u32 const index) -> Entry {
    auto entry = Entry{};
    std::memcpy(&entry, blob.data() + sizeof(bootfs::Header) + (index * sizeof(Entry)),
                sizeof(entry));
    return entry;
}

auto set_entry(std::vector<u8> &blob, u32 const index, Entry const &entry) -> void {
    std::memcpy(blob.data() + sizeof(bootfs::Header) + (index * sizeof(Entry)), &entry,
                sizeof(entry));
}

auto parse(std::vector<u8> const &blob) -> View::Result {
    return View::from({blob.data(), blob.size()});
}

auto expect_refused(std::vector<u8> const &blob, Status const status) -> void {
    auto const result = parse(blob);
    ASSERT_TRUE(result.is_err()) << "View::from() accepted a BootFS that breaks the page rule";
    EXPECT_EQ(static_cast<int>(result.unwrap_err()), static_cast<int>(status));
}

auto expect_accepted(std::vector<u8> const &blob) -> void {
    auto const result = parse(blob);
    ASSERT_TRUE(result.is_ok()) << "View::from() answered "
                                << static_cast<int>(result.unwrap_err());
}

/// Every file the Builder lays out comes back from the View under its name,
/// with its bytes, from data that starts on a page.
TEST(View, BuilderRoundTrip) {
    auto const files = std::vector<TestFile>{
        {.name = "bin/two-pages", .data = bytes_of((ALIGNMENT * 2) - 7, 1)},
        {.name = "empty", .data = {}},
        {.name = "data/one-page", .data = bytes_of(ALIGNMENT, 2)},
        {.name = "data/small", .data = bytes_of(100, 3)},
    };
    auto const blob = build(files);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob.size() % ALIGNMENT, 0U) << "the Builder pads the BootFS out to a page";

    auto const result = parse(blob);
    ASSERT_TRUE(result.is_ok()) << "View::from() answered "
                                << static_cast<int>(result.unwrap_err());
    auto const &view = result.unwrap();
    ASSERT_EQ(view.file_count(), files.size());
    EXPECT_EQ(view.total_size(), blob.size());

    for (u32 index = 0; index < view.file_count(); ++index) {
        auto const &file = files[index];
        auto const entry = view.file(index);
        ASSERT_TRUE(entry.is_ok());
        EXPECT_EQ(entry.unwrap().data_offset % ALIGNMENT, 0U) << file.name;
        EXPECT_EQ(view.file_name(index), file.name);

        auto const data = view.file_data(index);
        EXPECT_TRUE(std::ranges::equal(data, file.data)) << file.name;

        auto const found = view.find(file.name);
        ASSERT_TRUE(found.is_ok()) << file.name;
        ASSERT_TRUE(found.unwrap().is_some()) << file.name;
        EXPECT_EQ(found.unwrap().value().data_offset, entry.unwrap().data_offset) << file.name;
    }

    // The Builder gives an empty file the offset of the file after it, whose
    // first page that offset is on. The rule must let the two share it.
    EXPECT_EQ(entry_of(blob, 1).data_offset, entry_of(blob, 2).data_offset);
}

/// A file whose data does not start on a page cannot be sliced out of the
/// BootFS VMO. The entry's own alignment field allows the offset here, so
/// only the format's page refuses it.
TEST(View, RefusesDataOffAPage) {
    auto const files = std::vector<TestFile>{
        {.name = "a", .data = bytes_of(100, 1)},
        {.name = "b", .data = bytes_of(100, 2)},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.alignment = 8;
    set_entry(blob, 1, entry);
    expect_accepted(blob);

    entry.data_offset += 8;
    set_entry(blob, 1, entry);
    expect_refused(blob, Status::InvArgs);
}

/// A file without bytes covers no page, but its offset still has to be on
/// one: the first half of the rule holds for every entry.
TEST(View, RefusesAnEmptyFileOffAPage) {
    auto const files = std::vector<TestFile>{
        {.name = "a", .data = bytes_of(100, 1)},
        {.name = "empty", .data = {}},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.data_offset = entry_of(blob, 0).data_offset + 8;
    entry.alignment = 8;
    set_entry(blob, 1, entry);
    expect_refused(blob, Status::InvArgs);
}

/// A file that starts on a page another file's bytes reach into: the first
/// file's slice would carry the second's first bytes.
TEST(View, RefusesAFileStartingOnAnothersLastPage) {
    auto const files = std::vector<TestFile>{
        {.name = "bin/a", .data = bytes_of(ALIGNMENT + 100, 1)},
        {.name = "data/b", .data = bytes_of(100, 2)},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.data_offset = entry_of(blob, 0).data_offset + ALIGNMENT;
    set_entry(blob, 1, entry);
    expect_refused(blob, Status::InvArgs);
}

/// The same refusal with the entries the other way round: the earlier entry
/// is the one that was moved, so the pair is found from the later one.
TEST(View, RefusesOverlapWhicheverEntryComesFirst) {
    auto const files = std::vector<TestFile>{
        {.name = "data/b", .data = bytes_of(100, 1)},
        {.name = "bin/a", .data = bytes_of(ALIGNMENT + 100, 2)},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 0);
    entry.data_offset = entry_of(blob, 1).data_offset + ALIGNMENT;
    set_entry(blob, 0, entry);
    expect_refused(blob, Status::InvArgs);
}

/// Two files that start on the same page but differ in size are not one file
/// under two names: the shorter one's last page holds the longer one's next
/// bytes. Zircon's format allows sharing only at the same offset and size.
TEST(View, RefusesTwoSizesOnOnePage) {
    auto const files = std::vector<TestFile>{
        {.name = "bin/a", .data = bytes_of(100, 1)},
        {.name = "data/b", .data = bytes_of(200, 2)},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.data_offset = entry_of(blob, 0).data_offset;
    set_entry(blob, 1, entry);
    expect_refused(blob, Status::InvArgs);
}

/// One file under two names, which BootfsSvc gives one VMO.
TEST(View, AcceptsOneFileUnderTwoNames) {
    auto const files = std::vector<TestFile>{
        {.name = "lib/one", .data = bytes_of(ALIGNMENT + 100, 1)},
        {.name = "lib/two", .data = bytes_of(ALIGNMENT + 100, 2)},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.data_offset = entry_of(blob, 0).data_offset;
    set_entry(blob, 1, entry);
    expect_accepted(blob);

    auto const view = parse(blob).unwrap();
    EXPECT_TRUE(std::ranges::equal(view.file_data(0), view.file_data(1)));
}

/// An empty file shares no page, even at an offset inside another file.
TEST(View, AcceptsAnEmptyFileInsideAnother) {
    auto const files = std::vector<TestFile>{
        {.name = "bin/a", .data = bytes_of((ALIGNMENT * 2) + 100, 1)},
        {.name = "empty", .data = {}},
    };
    auto blob = build(files);

    auto entry = entry_of(blob, 1);
    entry.data_offset = entry_of(blob, 0).data_offset + ALIGNMENT;
    set_entry(blob, 1, entry);
    expect_accepted(blob);
}

/// A file that fills its last page ends where the next page starts, so the
/// file on that next page shares nothing with it.
TEST(View, AcceptsFilesOnAdjacentPages) {
    auto const files = std::vector<TestFile>{
        {.name = "bin/a", .data = bytes_of(ALIGNMENT, 1)},
        {.name = "data/b", .data = bytes_of(100, 2)},
    };
    auto const blob = build(files);
    ASSERT_EQ(entry_of(blob, 1).data_offset, entry_of(blob, 0).data_offset + ALIGNMENT);
    expect_accepted(blob);
}
} // namespace
