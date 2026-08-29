/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// phys::BootObi's boot contract: exactly one StorageKernel package, plus
/// an Executable entry-point item at index 0 (PackageBuilder's sort order).
/// obi itself only formats/reads the container -- this reading of it is
/// phys's own (see ours/phys/boot_obi.hpp); obi's own PackageBuilder/
/// ContainerView round-trip is covered separately by
/// //kernel/lib/obi/tests:package-builder-test.

#include <obi/container_view.hpp>
#include <obi/package_builder.hpp>
#include <ours/phys/boot_obi.hpp>

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
using ours::phys::BootObi;

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

// NOLINTNEXTLINE(readability-identifier-naming) — gtest TEST macro, file-wide pattern
TEST(BootObi, AcceptsExactlyOneKernelPackageAndExposesTheExecutable) {
    // NOLINTNEXTLINE(readability-identifier-naming) — matches the pre-existing kernel_bytes/setup_bytes fixtures
    constexpr auto elf_bytes = std::array<u8, 4>{0x7F, 'E', 'L', 'F'};
    // NOLINTNEXTLINE(readability-identifier-naming) — matches the pre-existing kernel_bytes/setup_bytes fixtures
    constexpr auto bootfs_bytes = std::array<u8, 3>{'b', 't', 's'};

    PackageBuilder builder;
    ASSERT_TRUE(builder
                    .add_item(item("physload", ItemType::Executable, ItemFormat::Elf,
                                   {elf_bytes.data(), elf_bytes.size()}))
                    .is_ok());
    ASSERT_TRUE(builder
                    .add_item(item("kernel", ItemType::StorageKernel, ItemFormat::BootFs,
                                   {bootfs_bytes.data(), bootfs_bytes.size()}))
                    .is_ok());

    auto const container = builder.build(/*crc=*/false);
    auto view = ContainerView::from(container.data(), container.size());
    ASSERT_TRUE(view.is_ok());

    auto boot = BootObi::from(view.unwrap());
    ASSERT_TRUE(boot.is_ok());
    auto const executable = boot.unwrap().executable();
    ASSERT_TRUE(executable.is_some());
    EXPECT_EQ(executable.value().name(), std::string_view("physload"));
}

// NOLINTNEXTLINE(readability-identifier-naming) — gtest TEST macro, file-wide pattern
TEST(BootObi, RejectsMissingKernelPackage) {
    // NOLINTNEXTLINE(readability-identifier-naming) — matches the pre-existing kernel_bytes/setup_bytes fixtures
    constexpr auto elf_bytes = std::array<u8, 4>{0x7F, 'E', 'L', 'F'};

    PackageBuilder builder;
    ASSERT_TRUE(builder
                    .add_item(item("physload", ItemType::Executable, ItemFormat::Elf,
                                   {elf_bytes.data(), elf_bytes.size()}))
                    .is_ok());

    auto const container = builder.build(/*crc=*/false);
    auto view = ContainerView::from(container.data(), container.size());
    ASSERT_TRUE(view.is_ok());

    auto boot = BootObi::from(view.unwrap());
    ASSERT_TRUE(boot.is_err());
    EXPECT_EQ(boot.unwrap_err(), ours::Status::NotFound);
}

// NOLINTNEXTLINE(readability-identifier-naming) — gtest TEST macro, file-wide pattern
TEST(BootObi, RejectsMultipleKernelPackages) {
    // NOLINTNEXTLINE(readability-identifier-naming) — matches the pre-existing kernel_bytes/setup_bytes fixtures
    constexpr auto bootfs_bytes = std::array<u8, 3>{'b', 't', 's'};

    PackageBuilder builder;
    ASSERT_TRUE(builder
                    .add_item(item("kernel", ItemType::StorageKernel, ItemFormat::BootFs,
                                   {bootfs_bytes.data(), bootfs_bytes.size()}))
                    .is_ok());
    ASSERT_TRUE(builder
                    .add_item(item("kernel-2", ItemType::StorageKernel, ItemFormat::BootFs,
                                   {bootfs_bytes.data(), bootfs_bytes.size()}))
                    .is_ok());

    auto const container = builder.build(/*crc=*/false);
    auto view = ContainerView::from(container.data(), container.size());
    ASSERT_TRUE(view.is_ok());

    auto boot = BootObi::from(view.unwrap());
    ASSERT_TRUE(boot.is_err());
    EXPECT_EQ(boot.unwrap_err(), ours::Status::AlreadyExists);
}
} // namespace
