/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_VERSION_DETAIL_HPP
#define EFTL_ELF_VERSION_DETAIL_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/encoding.hpp>

#include <limits>
#include <type_traits>

#include <ustl/option.hpp>

namespace eftl::elf::detail {
template <typename Traits, typename Entry>
[[nodiscard]]
auto read_version_record(ByteRange bytes, usize offset, ByteOrder byte_order) noexcept
    -> ustl::Option<Entry> {
    if (offset > bytes.size() || sizeof(Entry) > bytes.size() - offset) {
        return ustl::NONE;
    }
    auto decoder = &RecordCodec<Traits, Entry>::decode_little;
    if (byte_order != ByteOrder::Little) {
        decoder = &RecordCodec<Traits, Entry>::decode_big;
    }
    return ustl::some(decoder(bytes.data() + offset));
}

template <typename Value>
[[nodiscard]]
constexpr auto version_to_usize(Value value) noexcept -> ustl::Option<usize> {
    static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
    if constexpr (sizeof(Value) > sizeof(usize)) {
        if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
            return ustl::NONE;
        }
    }
    return ustl::some(static_cast<usize>(value));
}

template <typename Value>
[[nodiscard]]
auto version_relative_offset(usize origin, Value delta, usize minimum, usize size) noexcept
    -> ustl::Option<usize> {
    auto const delta_size = version_to_usize(delta);
    if (delta_size.is_none() || delta_size.value() < minimum || origin > size ||
        delta_size.value() > size - origin) {
        return ustl::NONE;
    }
    return ustl::some(origin + delta_size.value());
}
} // namespace eftl::elf::detail

#endif // EFTL_ELF_VERSION_DETAIL_HPP
