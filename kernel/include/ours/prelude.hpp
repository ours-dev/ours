#ifndef PRELUDE_HPP
#define PRELUDE_HPP

#include <cstddef>
#include <cstdint>

using i8 = ::std::int8_t;
using i16 = ::std::int16_t;
using i32 = ::std::int32_t;
using i64 = ::std::int64_t;
using isize = ::std::ptrdiff_t;

using u8 = ::std::uint8_t;
using u16 = ::std::uint16_t;
using u32 = ::std::uint32_t;
using u64 = ::std::uint64_t;
using usize = ::std::size_t;

constexpr auto operator""_i8(unsigned long long v) -> i8 {
    return i8(v);
}
constexpr auto operator""_u8(unsigned long long v) -> u8 {
    return u8(v);
}
constexpr auto operator""_i16(unsigned long long v) -> i16 {
    return i16(v);
}
constexpr auto operator""_u16(unsigned long long v) -> u16 {
    return u16(v);
}
constexpr auto operator""_i32(unsigned long long v) -> i32 {
    return i32(v);
}
constexpr auto operator""_u32(unsigned long long v) -> u32 {
    return u32(v);
}
constexpr auto operator""_i64(unsigned long long v) -> i64 {
    return i64(v);
}
constexpr auto operator""_u64(unsigned long long v) -> u64 {
    return u64(v);
}
constexpr auto operator""_isize(unsigned long long v) -> isize {
    return isize(v);
}
constexpr auto operator""_usize(unsigned long long v) -> usize {
    return usize(v);
}
constexpr auto operator""_bit(unsigned long long v) -> usize {
    return usize{1} << v;
}

constexpr auto operator""_kb(unsigned long long v) -> isize {
    return isize(v) << 10;
}
constexpr auto operator""_mb(unsigned long long v) -> isize {
    return isize(v) << 20;
}
constexpr auto operator""_gb(unsigned long long v) -> isize {
    return isize(v) << 30;
}
constexpr auto operator""_tb(unsigned long long v) -> isize {
    return isize(v) << 40;
}
constexpr auto operator""_pb(unsigned long long v) -> isize {
    return isize(v) << 40;
}

#endif // PRELUDE_HPP