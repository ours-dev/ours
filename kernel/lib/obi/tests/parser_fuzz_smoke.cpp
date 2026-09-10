#include <obi/compression.hpp>
#include <obi/container_view.hpp>
#include <obi/package_builder.hpp>

#include <array>

#include <cstdlib>
#include <cstring>

namespace {
using obi::Compression;
using obi::ContainerView;
using obi::ItemFlags;
using obi::ItemFormat;
using obi::ItemSource;
using obi::ItemType;
using obi::PackageBuilder;

auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto next_random(u64 &state) -> u8 {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return static_cast<u8>(state);
}

auto exercise_random_inputs() -> void {
    auto bytes = std::array<u8, 512>{};
    auto state = u64{0xA7E1D5C3B9F24680};
    for (usize iteration = 0; iteration < 1024; ++iteration) {
        for (auto &byte: bytes) {
            byte = next_random(state);
        }
        auto const size = static_cast<usize>(next_random(state));
        [[maybe_unused]]
        auto const parsed = ContainerView::from(bytes.data(), size);
    }
}

auto reject_every_single_bit_corruption() -> void {
    constexpr auto payload = std::array<u8, 5>{'o', 'b', 'i', '!', '\n'};
    PackageBuilder builder;
    auto const added = builder.add_item(ItemSource{
        .name = "payload",
        .type = ItemType::Data,
        .format = ItemFormat::Raw,
        .data = ustl::views::Span<u8 const>(payload.data(), payload.size()),
        .flags = ItemFlags::Required,
        .memory_size = payload.size(),
        .alignment = 8,
        .compression = Compression::None,
    });
    require(added.is_ok());

    auto const container = builder.build(/*crc=*/true);
    require(!container.empty());
    require(ContainerView::from(container.data(), container.size()).is_ok());

    for (usize byte = 0; byte < container.size(); ++byte) {
        for (u8 bit = 0; bit < 8; ++bit) {
            auto corrupted = container;
            corrupted[byte] ^= static_cast<u8>(1u << bit);
            require(ContainerView::from(corrupted.data(), corrupted.size()).is_err());
        }
    }
}

auto exercise_compression_inputs() -> void {
    auto input = std::array<u8, 512>{};
    auto output = std::array<u8, 1024>{};
    auto decoded = std::array<u8, 512>{};
    auto state = u64{0x51C0FFEE12345678};
    for (usize size = 0; size <= input.size(); size += 17) {
        for (usize index = 0; index < size; ++index) {
            input[index] = next_random(state);
        }
        auto const encoded_size = obi::compression::compressed_size(size);
        require(encoded_size.is_ok() && encoded_size.unwrap() <= output.size());
        auto const encoded = obi::compression::compress_literals(
            obi::compression::Bytes(input.data(), size),
            obi::compression::MutableBytes(output.data(), output.size()));
        require(encoded.is_ok() && encoded.unwrap() == encoded_size.unwrap());
        auto const decoded_size = obi::compression::decompress(
            obi::compression::Bytes(output.data(), encoded.unwrap()),
            obi::compression::MutableBytes(decoded.data(), decoded.size()));
        require(decoded_size.is_ok() && decoded_size.unwrap() == size);
        require(std::memcmp(decoded.data(), input.data(), size) == 0);
    }

    for (usize iteration = 0; iteration < 2048; ++iteration) {
        for (auto &byte: input) {
            byte = next_random(state);
        }
        auto const input_size = static_cast<usize>(next_random(state));
        auto const result = obi::compression::decompress(
            obi::compression::Bytes(input.data(), input_size),
            obi::compression::MutableBytes(decoded.data(), decoded.size()));
        [[maybe_unused]]
        auto const ignored = result;
    }
}
} // namespace

auto main() -> int {
    exercise_random_inputs();
    reject_every_single_bit_corruption();
    exercise_compression_inputs();
    return 0;
}
