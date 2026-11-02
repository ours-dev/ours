/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef WIRE_SHA256_HPP
#define WIRE_SHA256_HPP 1

#include <array>
#include <bit>
#include <string_view>

#include <ours/types.hpp>

/// SHA-256 (FIPS 180-4), written so that the compiler can evaluate it: the
/// ordinal of a wire::Method is hashed from its selector while the program is
/// compiled (wire::ordinal_of), so the hash has to run in a constant
/// expression. It is `constexpr` all through, so a test can also run it, and
/// it uses no heap, no mutable static state and nothing the standard library
/// leaves out of a freestanding build.
///
/// It is meant for short messages: selectors and test vectors. The compiler
/// bounds how much one constant expression may evaluate (clang's
/// -fconstexpr-steps, 1,048,576 by default), and a message pays for each of its
/// 64-byte blocks, about 6,500 steps with clang 23. A selector of 34 bytes, the
/// longest in the tree, takes 7,000 steps and one of 255 bytes 32,500, so none
/// is near the limit; a message of some 10 KB would be.
namespace wire {
/// A SHA-256 digest: the eight state words, each most significant byte first.
using Sha256Digest = std::array<u8, 32>;

namespace detail {
    /// The bytes a message is compressed in (FIPS 180-4, 1.1).
    inline constexpr auto SHA256_BLOCK_SIZE = usize{64};

    /// Where the message length starts in the last block of a padded message: the
    /// final 8 bytes of a block hold it, so a message whose last block holds more
    /// than this many bytes needs one more block for the padding (5.1.1).
    inline constexpr auto SHA256_LENGTH_OFFSET = SHA256_BLOCK_SIZE - 8;

    using Sha256State = std::array<u32, 8>;

    /// The state a message starts from: the first 32 bits of the fractional parts
    /// of the square roots of the first eight primes (5.3.3).
    inline constexpr auto SHA256_INITIAL_STATE = Sha256State{
        0x6a09'e667, 0xbb67'ae85, 0x3c6e'f372, 0xa54f'f53a,
        0x510e'527f, 0x9b05'688c, 0x1f83'd9ab, 0x5be0'cd19,
    };

    /// One constant per round: the first 32 bits of the fractional parts of the
    /// cube roots of the first 64 primes (4.2.2).
    inline constexpr auto SHA256_ROUND_CONSTANTS = std::array<u32, 64>{
        0x428a'2f98, 0x7137'4491, 0xb5c0'fbcf, 0xe9b5'dba5, 0x3956'c25b, 0x59f1'11f1, 0x923f'82a4,
        0xab1c'5ed5, 0xd807'aa98, 0x1283'5b01, 0x2431'85be, 0x550c'7dc3, 0x72be'5d74, 0x80de'b1fe,
        0x9bdc'06a7, 0xc19b'f174, 0xe49b'69c1, 0xefbe'4786, 0x0fc1'9dc6, 0x240c'a1cc, 0x2de9'2c6f,
        0x4a74'84aa, 0x5cb0'a9dc, 0x76f9'88da, 0x983e'5152, 0xa831'c66d, 0xb003'27c8, 0xbf59'7fc7,
        0xc6e0'0bf3, 0xd5a7'9147, 0x06ca'6351, 0x1429'2967, 0x27b7'0a85, 0x2e1b'2138, 0x4d2c'6dfc,
        0x5338'0d13, 0x650a'7354, 0x766a'0abb, 0x81c2'c92e, 0x9272'2c85, 0xa2bf'e8a1, 0xa81a'664b,
        0xc24b'8b70, 0xc76c'51a3, 0xd192'e819, 0xd699'0624, 0xf40e'3585, 0x106a'a070, 0x19a4'c116,
        0x1e37'6c08, 0x2748'774c, 0x34b0'bcb5, 0x391c'0cb3, 0x4ed8'aa4a, 0x5b9c'ca4f, 0x682e'6ff3,
        0x748f'82ee, 0x78a5'636f, 0x84c8'7814, 0x8cc7'0208, 0x90be'fffa, 0xa450'6ceb, 0xbef9'a3f7,
        0xc671'78f2,
    };

    /// The big-endian word at byte |offset| of |block|.
    [[nodiscard]]
    constexpr auto sha256_word(std::string_view const block, usize const offset) -> u32 {
        auto word = u32{0};
        for (auto i = usize{0}; i < 4; ++i) {
            // A char is signed on some targets; the byte is what it holds.
            word = (word << 8) | u32{static_cast<u8>(block[offset + i])};
        }
        return word;
    }

    /// Folds the SHA256_BLOCK_SIZE bytes of |block| into |state| (6.2.2).
    constexpr auto sha256_compress(Sha256State &state, std::string_view const block) -> void {
        // The message schedule: the block's 16 words, then 48 more mixed from
        // them.
        auto schedule = std::array<u32, 64>{};
        for (auto t = usize{0}; t < 16; ++t) {
            schedule[t] = sha256_word(block, 4 * t);
        }
        for (auto t = usize{16}; t < 64; ++t) {
            auto const w15 = schedule[t - 15];
            auto const w2 = schedule[t - 2];
            auto const sigma0 = std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3);
            auto const sigma1 = std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10);
            schedule[t] = schedule[t - 16] + sigma0 + schedule[t - 7] + sigma1;
        }

        auto a = state[0];
        auto b = state[1];
        auto c = state[2];
        auto d = state[3];
        auto e = state[4];
        auto f = state[5];
        auto g = state[6];
        auto h = state[7];
        for (auto t = usize{0}; t < 64; ++t) {
            auto const sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            auto const choose = (e & f) ^ (~e & g);
            auto const temp1 = h + sigma1 + choose + SHA256_ROUND_CONSTANTS[t] + schedule[t];
            auto const sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            auto const majority = (a & b) ^ (a & c) ^ (b & c);
            auto const temp2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
} // namespace detail

/// The SHA-256 digest of |message|, any length.
///
/// The whole 64-byte blocks of the message are compressed as they are. The
/// rest is copied into a buffer of two blocks and padded as FIPS 180-4, 5.1.1
/// says: a 1 bit (0x80), zeros, and the message's length in bits as 8
/// big-endian bytes at the end of the last block. The padding fits the rest's
/// own block when the rest is at most 55 bytes; from 56 it needs a block more.
[[nodiscard]]
constexpr auto sha256(std::string_view const message) -> Sha256Digest {
    auto state = detail::SHA256_INITIAL_STATE;

    auto const whole_blocks = message.size() / detail::SHA256_BLOCK_SIZE;
    for (auto i = usize{0}; i < whole_blocks; ++i) {
        detail::sha256_compress(
            state, message.substr(i * detail::SHA256_BLOCK_SIZE, detail::SHA256_BLOCK_SIZE));
    }

    auto const rest = message.size() - whole_blocks * detail::SHA256_BLOCK_SIZE;
    auto tail = std::array<char, 2 * detail::SHA256_BLOCK_SIZE>{};
    for (auto i = usize{0}; i < rest; ++i) {
        tail[i] = message[whole_blocks * detail::SHA256_BLOCK_SIZE + i];
    }
    tail[rest] = static_cast<char>(0x80);

    auto tail_size = detail::SHA256_BLOCK_SIZE;
    if (rest > detail::SHA256_LENGTH_OFFSET - 1) {
        tail_size = 2 * detail::SHA256_BLOCK_SIZE;
    }
    auto const bit_length = u64{message.size()} * 8;
    for (auto i = usize{0}; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<char>(bit_length >> (8 * i));
    }
    for (auto offset = usize{0}; offset < tail_size; offset += detail::SHA256_BLOCK_SIZE) {
        detail::sha256_compress(state,
                                std::string_view{tail.data() + offset, detail::SHA256_BLOCK_SIZE});
    }

    auto digest = Sha256Digest{};
    for (auto i = usize{0}; i < state.size(); ++i) {
        for (auto j = usize{0}; j < 4; ++j) {
            digest[4 * i + j] = static_cast<u8>(state[i] >> (24 - 8 * j));
        }
    }
    return digest;
}
} // namespace wire

#endif // #ifndef WIRE_SHA256_HPP
