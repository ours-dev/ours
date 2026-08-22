#include <lz4/lz4.hpp>

#include <array>

#include <cstdlib>
#include <cstring>

namespace {

auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}
} // namespace

auto main() -> int {
    auto input = std::array<u8, 300>{};
    for (usize index = 0; index < input.size(); ++index) {
        input[index] = static_cast<u8>((index * 37) & 0xFF);
    }
    auto encoded = std::array<u8, 512>{};
    auto decoded = std::array<u8, 300>{};
    auto const encoded_size =
        lz4::compress(input.data(), encoded.data(), input.size(), encoded.size());
    require(encoded_size > 0);
    require(lz4::calc_uncompressed_size(encoded.data(), static_cast<usize>(encoded_size)) ==
            input.size());
    auto const decoded_size = lz4::decompress(encoded.data(), decoded.data(),
                                              static_cast<usize>(encoded_size), decoded.size());
    require(decoded_size == static_cast<isize>(input.size()));
    require(std::memcmp(input.data(), decoded.data(), input.size()) == 0);

    constexpr auto matched = std::array<u8, 6>{0x32, 'a', 'b', 'c', 3, 0};
    auto matched_output = std::array<u8, 9>{};
    require(lz4::decompress(matched.data(), matched_output.data(), matched.size(),
                            matched_output.size()) == static_cast<isize>(matched_output.size()));
    constexpr auto expected = std::array<u8, 9>{'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'};
    require(matched_output == expected);

    auto fuzz_input = std::array<u8, 256>{};
    auto fuzz_output = std::array<u8, 512>{};
    auto state = u64{0xC0DEC0DE12345678};
    for (usize iteration = 0; iteration < 2048; ++iteration) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        for (auto &byte: fuzz_input) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            byte = static_cast<u8>(state);
        }
        auto const size = static_cast<usize>(state & 0xFF);
        [[maybe_unused]]
        auto const decoded_size = lz4::calc_uncompressed_size(fuzz_input.data(), size);
        [[maybe_unused]]
        auto const result =
            lz4::decompress(fuzz_input.data(), fuzz_output.data(), size, fuzz_output.size());
    }
    return 0;
}
