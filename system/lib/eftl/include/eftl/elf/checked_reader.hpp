/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_CHECKED_READER_HPP
#define EFTL_ELF_CHECKED_READER_HPP 1

#include <eftl/binary_stream.hpp>

#include <limits>
#include <type_traits>

#include <cstring>

namespace eftl::elf {
template <typename T>
struct WireObjectTraits: std::false_type {};

struct ElfSiginfo;

struct Elf32Ehdr;
struct Elf32Phdr;
struct Elf32Shdr;
struct Elf32Sym;
struct Elf32Auxv;
struct Elf32Dyn;
struct Elf32Rel;
struct Elf32Rela;
struct Elf32Verneed;
struct Elf32Verdef;
struct Elf32Verdaux;
struct Elf32Vernaux;
struct Elf32Prstatus;
struct Elf32FileEntry;
struct Elf32Timeval;

struct Elf64Ehdr;
struct Elf64Phdr;
struct Elf64Shdr;
struct Elf64Sym;
struct Elf64Auxv;
struct Elf64Dyn;
struct Elf64Rel;
struct Elf64Rela;
struct Elf64Verneed;
struct Elf64Verdef;
struct Elf64Verdaux;
struct Elf64Vernaux;
struct Elf64Prstatus;
struct Elf64FileEntry;
struct Elf64Timeval;

template <>
struct WireObjectTraits<ElfSiginfo>: std::true_type {};

template <>
struct WireObjectTraits<Elf32Ehdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Phdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Shdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Sym>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Auxv>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Dyn>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Rel>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Rela>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Verneed>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Verdef>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Verdaux>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Vernaux>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Prstatus>: std::true_type {};
template <>
struct WireObjectTraits<Elf32FileEntry>: std::true_type {};
template <>
struct WireObjectTraits<Elf32Timeval>: std::true_type {};

template <>
struct WireObjectTraits<Elf64Ehdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Phdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Shdr>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Sym>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Auxv>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Dyn>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Rel>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Rela>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Verneed>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Verdef>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Verdaux>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Vernaux>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Prstatus>: std::true_type {};
template <>
struct WireObjectTraits<Elf64FileEntry>: std::true_type {};
template <>
struct WireObjectTraits<Elf64Timeval>: std::true_type {};

template <typename T>
concept WireObject = std::is_same_v<T, std::remove_cv_t<T>> && std::is_trivially_copyable_v<T> &&
                     ((std::is_integral_v<T> && std::is_unsigned_v<T> &&
                       !std::is_same_v<std::remove_cv_t<T>, bool>) ||
                      WireObjectTraits<std::remove_cv_t<T>>::value);

enum class ReaderError {
    NullData,
    OutOfRange,
    NonTrivialType,
};

using ByteRange = eftl::ByteRange;

class CheckedReader final {
  public:
    CheckedReader() noexcept = default;

    [[nodiscard]]
    static auto from(void const *data, usize size) -> ustl::Result<CheckedReader, ReaderError> {
        auto const stream = BinaryStream::from(data, size);
        if (stream.is_err()) {
            return ustl::err(reader_error(stream.unwrap_err()));
        }
        return ustl::ok(CheckedReader(stream.unwrap()));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_stream.size();
    }

    [[nodiscard]]
    auto bytes(usize offset, usize size) const -> ustl::Result<ByteRange, ReaderError> {
        auto const range = m_stream.bytes(offset, size);
        if (range.is_err()) {
            return ustl::err(reader_error(range.unwrap_err()));
        }
        return ustl::ok(range.unwrap());
    }

    template <typename T>
    [[nodiscard]]
    auto object(usize offset) const -> ustl::Result<T, ReaderError> {
        static_assert(WireObject<T>, "ELF object types must be valid wire objects");
        auto const range = bytes(offset, sizeof(T));
        if (range.is_err()) {
            return ustl::err(range.unwrap_err());
        }

        T value{};
        std::memcpy(&value, range.unwrap().data(), sizeof(T));
        return ustl::ok(value);
    }

    template <typename T>
    [[nodiscard]]
    auto array(usize offset, usize count) const -> ustl::Result<ByteRange, ReaderError> {
        static_assert(std::is_trivially_copyable_v<T>, "ELF objects must be trivially copyable");
        if (count > std::numeric_limits<usize>::max() / sizeof(T)) {
            return ustl::err(ReaderError::OutOfRange);
        }
        return bytes(offset, count * sizeof(T));
    }

  private:
    constexpr explicit CheckedReader(BinaryStream stream) noexcept : m_stream(stream) {}

    [[nodiscard]]
    static constexpr auto reader_error(StreamError error) noexcept -> ReaderError {
        switch (error) {
            case StreamError::NullData:
                return ReaderError::NullData;
            case StreamError::OutOfRange:
                return ReaderError::OutOfRange;
            default:
                return ReaderError::OutOfRange;
        }
    }

    BinaryStream m_stream{};
};
} // namespace eftl::elf

#endif // EFTL_ELF_CHECKED_READER_HPP
