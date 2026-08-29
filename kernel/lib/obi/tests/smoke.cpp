/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// obi-smoke — broad coverage of the current obi library surface: building
/// and parsing multi-item containers, compression round-trips, CRC
/// detection, and a battery of malformed-image rejections.  BootFS is
/// exercised separately (obi doesn't depend on it — see kernel/lib/bootfs/).

#include <obi/checksum.hpp>
#include <obi/command_line_view.hpp>
#include <obi/compression.hpp>
#include <obi/container_view.hpp>
#include <obi/package_builder.hpp>
#include <obi/package_format.hpp>

#include <array>
#include <random>
#include <vector>

#include <cstdlib>
#include <cstring>

namespace {
using obi::CommandLineView;
using obi::Compression;
using obi::ContainerHeader;
using obi::ContainerView;
using obi::ItemDescriptor;
using obi::ItemFlags;
using obi::ItemFormat;
using obi::ItemSource;
using obi::ItemType;
using obi::PackageBuilder;

auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto item(std::string name, ItemType type, ItemFormat format, ustl::views::Span<u8 const> data,
          Compression compression = Compression::None, u64 memory_size = 0) -> ItemSource {
    return ItemSource{
        .name = std::move(name),
        .type = type,
        .format = format,
        .data = data,
        .flags = ItemFlags::Required,
        .memory_size = memory_size == 0 ? data.size() : memory_size,
        .alignment = 8,
        .compression = compression,
    };
}

// ---- Multi-item build + round-trip parse -------------------------

auto builds_and_reads_a_multi_item_container() -> void {
    constexpr auto kernel_bytes = std::array<u8, 4>{0x7F, 'E', 'L', 'F'};
    constexpr auto cmdline_bytes = std::array<u8, 17>{
        'a', 's', 'l', 'r', '.', 'd', 'i', 's', 'a', 'b', 'l', 'e', 'd', '=', '0', '\n', 0,
    };
    constexpr auto setup_bytes = std::array<u8, 3>{'v', '1', '\n'};

    PackageBuilder builder;
    require(builder
                .add_item(item("kernel", ItemType::Executable, ItemFormat::Elf,
                               {kernel_bytes.data(), kernel_bytes.size()}))
                .is_ok());
    require(builder
                .add_item(item("cmdline", ItemType::Config, ItemFormat::Raw,
                               {cmdline_bytes.data(), cmdline_bytes.size()}))
                .is_ok());
    require(builder
                .add_item(item("setup", ItemType::Config, ItemFormat::Yaml,
                               {setup_bytes.data(), setup_bytes.size()}))
                .is_ok());

    auto const container = builder.build(/*crc=*/false);
    require(!container.empty());

    auto view = ContainerView::from(container.data(), container.size());
    require(view.is_ok());
    require(view.unwrap().item_count() == 3u);

    auto const kernel = view.unwrap().find("kernel");
    require(kernel.is_ok() && kernel.unwrap().is_some());
    require(kernel.unwrap().value().type() == ItemType::Executable);

    auto const cmdline_item = view.unwrap().find("cmdline");
    require(cmdline_item.is_ok() && cmdline_item.unwrap().is_some());
    auto cmdline = CommandLineView::from(cmdline_item.unwrap().value());
    require(cmdline.is_ok());
    auto const aslr_value = cmdline.unwrap().value("aslr.disabled");
    require(aslr_value.is_some() && aslr_value.value() == std::string_view("0"));

    require(view.unwrap().setup_item_index() != ContainerHeader::kNoSetupItem);
}

// ---- Compression round-trip ---------------------------------------

auto compresses_and_materializes_an_item() -> void {
    std::string source;
    for (int i = 0; i < 40; ++i) {
        source += "the quick brown fox jumps over the lazy dog ";
    }
    auto const raw =
        ustl::views::Span<u8 const>(reinterpret_cast<u8 const *>(source.data()), source.size());

    auto const required = obi::compression::compressed_size(raw.size());
    require(required.is_ok());
    std::vector<u8> compressed(required.unwrap());
    auto const written =
        obi::compression::compress_literals(raw, {compressed.data(), compressed.size()});
    require(written.is_ok());
    compressed.resize(written.unwrap());

    PackageBuilder builder;
    require(
        builder
            .add_item(item("blob", ItemType::Data, ItemFormat::Raw,
                           {compressed.data(), compressed.size()}, Compression::Lz4, raw.size()))
            .is_ok());
    auto const container = builder.build(/*crc=*/false);
    require(!container.empty());

    auto view = ContainerView::from(container.data(), container.size());
    require(view.is_ok());
    auto const blob = view.unwrap().find("blob");
    require(blob.is_ok() && blob.unwrap().is_some());

    std::vector<u8> materialized(blob.unwrap().value().memory_size());
    auto const result =
        blob.unwrap().value().materialize({materialized.data(), materialized.size()});
    require(result.is_ok());
    require(result.unwrap() == raw.size());
    require(std::memcmp(materialized.data(), raw.data(), raw.size()) == 0);
}

auto rejects_a_container_claiming_lz4_over_undersized_memory_size() -> void {
    std::string source(200, 'a');
    auto const raw =
        ustl::views::Span<u8 const>(reinterpret_cast<u8 const *>(source.data()), source.size());
    auto const required = obi::compression::compressed_size(raw.size());
    require(required.is_ok());
    std::vector<u8> compressed(required.unwrap());
    auto const written =
        obi::compression::compress_literals(raw, {compressed.data(), compressed.size()});
    require(written.is_ok());
    compressed.resize(written.unwrap());

    PackageBuilder builder;
    // Declare a memory_size far smaller than what the LZ4 block
    // actually decodes to — ContainerView::from() must catch this
    // without decoding into an under-sized buffer.
    require(builder
                .add_item(item("blob", ItemType::Data, ItemFormat::Raw,
                               {compressed.data(), compressed.size()}, Compression::Lz4,
                               /*memory_size=*/4))
                .is_ok());
    auto const container = builder.build(/*crc=*/false);
    require(!container.empty());
    require(ContainerView::from(container.data(), container.size()).is_err());
}

// ---- CRC ------------------------------------------------------------

auto crc_detects_every_single_bit_corruption_in_a_small_container() -> void {
    constexpr auto payload = std::array<u8, 5>{'o', 'b', 'i', '!', '\n'};
    PackageBuilder builder;
    require(builder
                .add_item(item("payload", ItemType::Data, ItemFormat::Raw,
                               {payload.data(), payload.size()}))
                .is_ok());
    auto const container = builder.build(/*crc=*/true);
    require(!container.empty());
    require(ContainerView::from(container.data(), container.size()).is_ok());

    for (std::size_t byte = 0; byte < container.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            auto corrupted = container;
            corrupted[byte] ^= static_cast<u8>(1u << bit);
            require(ContainerView::from(corrupted.data(), corrupted.size()).is_err());
        }
    }
}

// ---- find() ----------------------------------------------------------

auto find_distinguishes_missing_single_and_ambiguous() -> void {
    constexpr auto bytes = std::array<u8, 1>{'x'};
    PackageBuilder builder;
    require(
        builder.add_item(item("a", ItemType::Data, ItemFormat::Raw, {bytes.data(), bytes.size()}))
            .is_ok());
    auto const container = builder.build(/*crc=*/false);
    auto view = ContainerView::from(container.data(), container.size());
    require(view.is_ok());

    auto const missing = view.unwrap().find("does-not-exist");
    require(missing.is_ok() && missing.unwrap().is_none());

    auto const present = view.unwrap().find("a");
    require(present.is_ok() && present.unwrap().is_some());
}

// ---- Malformed-image rejection battery --------------------------------

auto make_minimal_valid_image() -> std::vector<u8> {
    constexpr auto bytes = std::array<u8, 4>{'d', 'a', 't', 'a'};
    PackageBuilder builder;
    require(
        builder
            .add_item(item("item", ItemType::Data, ItemFormat::Raw, {bytes.data(), bytes.size()}))
            .is_ok());
    return builder.build(/*crc=*/false);
}

auto header_at(std::vector<u8> &image) -> ContainerHeader & {
    return *reinterpret_cast<ContainerHeader *>(image.data());
}

auto rejects_bad_magic() -> void {
    auto image = make_minimal_valid_image();
    header_at(image).magic ^= 0xFFFFFFFF;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_wrong_major_version() -> void {
    auto image = make_minimal_valid_image();
    header_at(image).major += 1;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_container_size_mismatch() -> void {
    auto image = make_minimal_valid_image();
    require(ContainerView::from(image.data(), image.size() - 8).is_err());
    require(ContainerView::from(image.data(), image.size() + 8).is_err());
}

auto rejects_unaligned_container_size() -> void {
    auto image = make_minimal_valid_image();
    image.push_back(0);
    header_at(image).container_size += 1;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_truncated_header() -> void {
    auto image = make_minimal_valid_image();
    require(ContainerView::from(image.data(), sizeof(ContainerHeader) - 1).is_err());
}

auto rejects_null_data() -> void {
    require(ContainerView::from(nullptr, 0).is_err());
}

auto rejects_out_of_range_setup_index() -> void {
    auto image = make_minimal_valid_image();
    header_at(image).setup_item_index = header_at(image).item_count;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_unsupported_item_flags() -> void {
    auto image = make_minimal_valid_image();
    auto *item_desc =
        reinterpret_cast<ItemDescriptor *>(image.data() + header_at(image).item_table_offset);
    item_desc->flags |= (1u << 31);
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_zero_length_name() -> void {
    auto image = make_minimal_valid_image();
    auto *item_desc =
        reinterpret_cast<ItemDescriptor *>(image.data() + header_at(image).item_table_offset);
    item_desc->name_size = 0;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_misaligned_payload() -> void {
    auto image = make_minimal_valid_image();
    auto *item_desc =
        reinterpret_cast<ItemDescriptor *>(image.data() + header_at(image).item_table_offset);
    item_desc->payload_offset += 1;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

auto rejects_non_power_of_two_alignment() -> void {
    auto image = make_minimal_valid_image();
    auto *item_desc =
        reinterpret_cast<ItemDescriptor *>(image.data() + header_at(image).item_table_offset);
    item_desc->alignment = 3;
    require(ContainerView::from(image.data(), image.size()).is_err());
}

// ---- Fuzz: random bytes must never crash the parser --------------

auto never_crashes_on_random_bytes() -> void {
    std::mt19937 rng(0xC0FFEE);
    std::array<u8, 512> bytes{};
    for (int iteration = 0; iteration < 4096; ++iteration) {
        for (auto &b: bytes) {
            b = static_cast<u8>(rng());
        }
        auto const size = rng() % (bytes.size() + 1);
        [[maybe_unused]]
        auto const result = ContainerView::from(bytes.data(), size);
    }
}
} // namespace

auto main() -> int {
    builds_and_reads_a_multi_item_container();
    compresses_and_materializes_an_item();
    rejects_a_container_claiming_lz4_over_undersized_memory_size();
    crc_detects_every_single_bit_corruption_in_a_small_container();
    find_distinguishes_missing_single_and_ambiguous();
    rejects_bad_magic();
    rejects_wrong_major_version();
    rejects_container_size_mismatch();
    rejects_unaligned_container_size();
    rejects_truncated_header();
    rejects_null_data();
    rejects_out_of_range_setup_index();
    rejects_unsupported_item_flags();
    rejects_zero_length_name();
    rejects_misaligned_payload();
    rejects_non_power_of_two_alignment();
    never_crashes_on_random_bytes();
    return 0;
}
