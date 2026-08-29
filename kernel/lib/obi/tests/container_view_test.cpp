/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <obi/container_view.hpp>
#include <obi/package_format.hpp>

#include <algorithm>
#include <array>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>

namespace {
using obi::Compression;
using obi::ContainerHeader;
using obi::ContainerView;
using obi::ItemDescriptor;
using obi::ItemFormat;
using obi::ItemType;

constexpr auto kImageSize = std::size_t{1024};

auto write_string(std::vector<u8> &strings, char const *value) -> std::pair<u32, u16> {
    auto const offset = static_cast<u32>(strings.size());
    auto const size = static_cast<u16>(std::strlen(value));
    strings.insert(strings.end(), value, value + size);
    return {offset, size};
}

auto make_image() -> std::array<u8, kImageSize> {
    alignas(ContainerHeader) std::array<u8, kImageSize> image{};
    std::vector<u8> strings;
    auto const [config_name_offset, config_name_size] = write_string(strings, "cmdline");
    auto const [setup_name_offset, setup_name_size] = write_string(strings, "setup");

    auto *header = reinterpret_cast<ContainerHeader *>(image.data());
    header->magic = ContainerHeader::kMagic;
    header->major = ContainerHeader::kMajor;
    header->minor = ContainerHeader::kMinor;
    header->header_size = sizeof(ContainerHeader);
    header->container_size = image.size();
    header->item_table_offset = sizeof(ContainerHeader);
    header->item_count = 2;
    header->string_table_offset =
        static_cast<u32>(sizeof(ContainerHeader) + 2 * sizeof(ItemDescriptor));
    header->string_table_size = static_cast<u32>(strings.size());
    header->setup_item_index = 1;

    std::memcpy(image.data() + header->string_table_offset, strings.data(), strings.size());

    auto *items = reinterpret_cast<ItemDescriptor *>(image.data() + header->item_table_offset);
    items[0] = {};
    items[0].name_offset = config_name_offset;
    items[0].name_size = config_name_size;
    items[0].type = static_cast<u32>(ItemType::Config);
    items[0].format = static_cast<u32>(ItemFormat::Raw);
    items[0].compression = static_cast<u32>(Compression::None);
    items[0].payload_offset = 304;
    items[0].payload_size = 4;
    items[0].memory_size = 4;
    items[0].alignment = 8;

    items[1] = {};
    items[1].name_offset = setup_name_offset;
    items[1].name_size = setup_name_size;
    items[1].type = static_cast<u32>(ItemType::Config);
    items[1].format = static_cast<u32>(ItemFormat::Yaml);
    items[1].compression = static_cast<u32>(Compression::None);
    items[1].payload_offset = 312;
    items[1].payload_size = 4;
    items[1].memory_size = 4;
    items[1].alignment = 8;

    std::memcpy(image.data() + items[0].payload_offset, "CFG!", 4);
    std::memcpy(image.data() + items[1].payload_offset, "v1\n\0", 4);
    return image;
}

TEST(ContainerView, ReadsItemsAndNamesFromValidContainer) {
    auto image = make_image();

    auto result = ContainerView::from(image.data(), image.size());

    ASSERT_TRUE(result.is_ok());
    auto const view = result.unwrap();
    ASSERT_EQ(view.item_count(), 2u);
    auto const item = view.item(0);
    ASSERT_TRUE(item.is_ok());
    EXPECT_EQ(item.unwrap().payload_size(), 4u);
    EXPECT_EQ(item.unwrap().name(), "cmdline");
    EXPECT_EQ(item.unwrap().type(), ItemType::Config);
}

TEST(ContainerView, RejectsItemPayloadOutsideContainer) {
    auto image = make_image();
    auto *item = reinterpret_cast<ItemDescriptor *>(image.data() + sizeof(ContainerHeader));
    item->payload_offset = image.size() - 2;
    item->payload_size = 4;

    auto const result = ContainerView::from(image.data(), image.size());

    EXPECT_TRUE(result.is_err());
}

TEST(ContainerView, FindLocatesItemsByName) {
    auto image = make_image();

    auto result = ContainerView::from(image.data(), image.size());
    ASSERT_TRUE(result.is_ok());
    auto const view = result.unwrap();

    auto const cmdline = view.find("cmdline");
    ASSERT_TRUE(cmdline.is_ok());
    ASSERT_TRUE(cmdline.unwrap().is_some());
    EXPECT_EQ(cmdline.unwrap().value().format(), ItemFormat::Raw);

    auto const setup = view.find("setup");
    ASSERT_TRUE(setup.is_ok());
    ASSERT_TRUE(setup.unwrap().is_some());
    EXPECT_EQ(setup.unwrap().value().format(), ItemFormat::Yaml);

    auto const missing = view.find("does-not-exist");
    ASSERT_TRUE(missing.is_ok());
    EXPECT_TRUE(missing.unwrap().is_none());
}

TEST(ContainerView, FindReportsAmbiguousNames) {
    auto image = make_image();
    // Rename the second item to collide with the first.
    auto *header = reinterpret_cast<ContainerHeader *>(image.data());
    auto *items = reinterpret_cast<ItemDescriptor *>(image.data() + header->item_table_offset);
    items[1].name_offset = items[0].name_offset;
    items[1].name_size = items[0].name_size;

    auto result = ContainerView::from(image.data(), image.size());
    ASSERT_TRUE(result.is_ok());
    auto const view = result.unwrap();

    auto const found = view.find("cmdline");
    EXPECT_TRUE(found.is_err());
}
} // namespace
