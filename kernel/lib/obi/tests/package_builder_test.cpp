/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <obi/container_view.hpp>
#include <obi/package_builder.hpp>

#include <array>
#include <string_view>

#include <gtest/gtest.h>

namespace {
using obi::Compression;
using obi::ContainerView;
using obi::ItemFlags;
using obi::ItemFormat;
using obi::ItemSource;
using obi::ItemType;
using obi::PackageBuilder;

auto item(std::string name, ItemType type, ItemFormat format, ustl::views::Span<u8 const> data)
    -> ItemSource {
    return ItemSource{
        .name = std::move(name),
        .type = type,
        .format = format,
        .data = data,
        .flags = ItemFlags::Required,
        .memory_size = data.size(),
        .alignment = 8,
        .compression = Compression::None,
    };
}

TEST(PackageBuilder, BuildsAContainerThatContainerViewCanRead) {
    constexpr auto kernel_bytes = std::array<u8, 4>{0x7F, 'E', 'L', 'F'};
    constexpr auto setup_bytes = std::array<u8, 3>{'v', '1', '\n'};

    PackageBuilder builder;
    ASSERT_TRUE(builder
                    .add_item(item("kernel", ItemType::Executable, ItemFormat::Elf,
                                   {kernel_bytes.data(), kernel_bytes.size()}))
                    .is_ok());
    ASSERT_TRUE(builder
                    .add_item(item("setup", ItemType::Config, ItemFormat::Yaml,
                                   {setup_bytes.data(), setup_bytes.size()}))
                    .is_ok());

    auto const container = builder.build(/*crc=*/false);
    ASSERT_FALSE(container.empty());
    EXPECT_EQ(container.size() % obi::ContainerHeader::ALIGNMENT, 0u);

    auto view = ContainerView::from(container.data(), container.size());
    ASSERT_TRUE(view.is_ok());
    EXPECT_EQ(view.unwrap().item_count(), 2u);

    auto const kernel = view.unwrap().find("kernel");
    ASSERT_TRUE(kernel.is_ok() && kernel.unwrap().is_some());
    EXPECT_EQ(kernel.unwrap().value().format(), ItemFormat::Elf);

    // "setup" is named+typed exactly as PackageBuilder's setup-index
    // convention expects (name=="setup", Config/Yaml), so build() must
    // have pointed setup_item_index at it automatically.
    EXPECT_NE(view.unwrap().setup_item_index(), obi::ContainerHeader::NO_SETUP_ITEM);
}

TEST(PackageBuilder, RejectsDuplicateItemNames) {
    constexpr auto bytes = std::array<u8, 1>{'x'};
    PackageBuilder builder;
    ASSERT_TRUE(
        builder.add_item(item("dup", ItemType::Data, ItemFormat::Raw, {bytes.data(), bytes.size()}))
            .is_ok());
    EXPECT_TRUE(
        builder.add_item(item("dup", ItemType::Data, ItemFormat::Raw, {bytes.data(), bytes.size()}))
            .is_err());
}

TEST(PackageBuilder, RejectsEmptyName) {
    constexpr auto bytes = std::array<u8, 1>{'x'};
    PackageBuilder builder;
    EXPECT_TRUE(
        builder.add_item(item("", ItemType::Data, ItemFormat::Raw, {bytes.data(), bytes.size()}))
            .is_err());
}

TEST(PackageBuilder, CrcRoundTripsAndDetectsCorruption) {
    constexpr auto bytes = std::array<u8, 6>{'p', 'a', 'y', 'l', 'o', 'a'};
    PackageBuilder builder;
    ASSERT_TRUE(builder
                    .add_item(item("payload", ItemType::Data, ItemFormat::Raw,
                                   {bytes.data(), bytes.size()}))
                    .is_ok());

    auto container = builder.build(/*crc=*/true);
    ASSERT_FALSE(container.empty());
    EXPECT_TRUE(ContainerView::from(container.data(), container.size()).is_ok());

    container[container.size() - 1] ^= 0xFF;
    EXPECT_TRUE(ContainerView::from(container.data(), container.size()).is_err());
}

TEST(PackageBuilder, EmptyBuilderProducesNothing) {
    PackageBuilder builder;
    EXPECT_TRUE(builder.build(/*crc=*/false).empty());
}
} // namespace

// The boot contract itself (exactly one StorageKernel package, an
// Executable entry point at item 0) is phys::BootObi's reading of this
// container format, not part of the format -- see
// //kernel/phys/boot_obi_test.cpp.
