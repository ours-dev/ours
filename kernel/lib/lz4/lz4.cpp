#include <limits>

#include <lz4/lz4.hpp>

namespace lz4 {
namespace {
    auto append_length(unsigned char const *input, usize input_size, usize &input_offset,
                       usize &length, bool extended) -> bool {
        if (!extended) {
            return true;
        }
        for (;;) {
            if (input_offset >= input_size) {
                return false;
            }
            auto const extension = static_cast<usize>(input[input_offset++]);
            if (length > std::numeric_limits<usize>::max() - extension) {
                return false;
            }
            length += extension;
            if (extension != 255) {
                return true;
            }
        }
    }

    auto decoded_size(void const *source, usize source_size) -> usize {
        auto const *input = static_cast<unsigned char const *>(source);
        if (input == nullptr && source_size != 0) {
            return 0;
        }
        usize input_offset = 0;
        usize output_size = 0;
        while (input_offset < source_size) {
            auto const token = input[input_offset++];
            usize literal_length = token >> 4;
            if (!append_length(input, source_size, input_offset, literal_length,
                               (token >> 4) == 15) ||
                literal_length > source_size - input_offset) {
                return 0;
            }
            input_offset += literal_length;
            if (output_size > std::numeric_limits<usize>::max() - literal_length) {
                return 0;
            }
            output_size += literal_length;
            if (input_offset == source_size) {
                return output_size;
            }
            if (source_size - input_offset < 2) {
                return 0;
            }
            auto const offset = static_cast<usize>(input[input_offset]) |
                                static_cast<usize>(input[input_offset + 1]) << 8;
            input_offset += 2;
            if (offset == 0 || offset > output_size) {
                return 0;
            }
            usize match_length = static_cast<usize>(token & 0x0F) + 4;
            if (!append_length(input, source_size, input_offset, match_length,
                               (token & 0x0F) == 15) ||
                output_size > std::numeric_limits<usize>::max() - match_length) {
                return 0;
            }
            output_size += match_length;
        }
        return output_size;
    }
} // namespace

auto compress(void const *source, void *destination, usize source_size, usize destination_size)
    -> isize {
    auto const *input = static_cast<unsigned char const *>(source);
    auto *output = static_cast<unsigned char *>(destination);
    if ((input == nullptr && source_size != 0) || output == nullptr) {
        return -1;
    }
    auto const extension = source_size < 15 ? usize{0} : (source_size - 15) / 255 + 1;
    if (source_size > std::numeric_limits<usize>::max() - extension - 1 ||
        destination_size < 1 + extension + source_size) {
        return -1;
    }
    usize output_offset = 0;
    output[output_offset++] =
        static_cast<unsigned char>((source_size < 15 ? source_size : 15) << 4);
    if (source_size >= 15) {
        auto remaining = source_size - 15;
        while (remaining >= 255) {
            output[output_offset++] = 255;
            remaining -= 255;
        }
        output[output_offset++] = static_cast<unsigned char>(remaining);
    }
    for (usize index = 0; index < source_size; ++index) {
        output[output_offset++] = input[index];
    }
    if (output_offset > static_cast<usize>(std::numeric_limits<isize>::max())) {
        return -1;
    }
    return static_cast<isize>(output_offset);
}

auto decompress(void const *source, void *destination, usize source_size, usize destination_size)
    -> isize {
    auto const *input = static_cast<unsigned char const *>(source);
    auto *output = static_cast<unsigned char *>(destination);
    if ((input == nullptr && source_size != 0) || (output == nullptr && destination_size != 0)) {
        return -1;
    }
    usize input_offset = 0;
    usize output_offset = 0;
    while (input_offset < source_size) {
        auto const token = input[input_offset++];
        usize literal_length = token >> 4;
        if (!append_length(input, source_size, input_offset, literal_length, (token >> 4) == 15) ||
            literal_length > source_size - input_offset ||
            literal_length > destination_size - output_offset) {
            return -1;
        }
        for (usize index = 0; index < literal_length; ++index) {
            output[output_offset++] = input[input_offset++];
        }
        if (input_offset == source_size) {
            break;
        }
        if (source_size - input_offset < 2) {
            return -1;
        }
        auto const offset = static_cast<usize>(input[input_offset]) |
                            static_cast<usize>(input[input_offset + 1]) << 8;
        input_offset += 2;
        if (offset == 0 || offset > output_offset) {
            return -1;
        }
        usize match_length = static_cast<usize>(token & 0x0F) + 4;
        if (!append_length(input, source_size, input_offset, match_length, (token & 0x0F) == 15) ||
            match_length > destination_size - output_offset) {
            return -1;
        }
        auto const match_offset = output_offset - offset;
        for (usize index = 0; index < match_length; ++index) {
            output[output_offset++] = output[match_offset + index % offset];
        }
    }
    if (output_offset > static_cast<usize>(std::numeric_limits<isize>::max())) {
        return -1;
    }
    return static_cast<isize>(output_offset);
}

auto calc_uncompressed_size(void const *source, usize source_size) -> usize {
    return decoded_size(source, source_size);
}
} // namespace lz4
