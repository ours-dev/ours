/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FSIO_PATH_HPP
#define FSIO_PATH_HPP 1

#include <array>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours_io/ours_io.hpp>

namespace fsio {
/// The longest path fsio takes: the longest one Open carries
/// (ours_io::MAX_PATH_LENGTH), which leaves room in a 4 KiB channel message
/// for Open's other fields.
///
/// A path this long still fits in an Open once cleaned, because cleaning never
/// lengthens a path (see clean_path()), and the part of it that Open carries,
/// what is left after its mount point, is shorter again.
inline constexpr auto MAX_PATH_LENGTH = ours_io::MAX_PATH_LENGTH;

/// A path as clean_path() leaves it, held in place: MAX_PATH_LENGTH bytes,
/// wherever the caller puts it, since a user program has no heap.
///
/// Only clean_path() writes one, so a function that takes a PathBuffer takes a
/// path that has been cleaned: Namespace::resolve() relies on that. A
/// PathBuffer nothing has written to holds the empty path, which clean_path()
/// never produces and which names nothing.
class PathBuffer final {
  public:
    constexpr PathBuffer() = default;

    [[nodiscard]]
    auto view() const -> std::string_view {
        return {m_chars.data(), m_size};
    }

  private:
    friend auto clean_path(std::string_view path, PathBuffer &out) -> ours::Status;

    std::array<char, MAX_PATH_LENGTH> m_chars{};
    usize m_size = 0;
};

/// Cleans |path| into |out| by the rules of Plan 9's "Lexical File Names in
/// Plan 9 or Getting Dot-Dot Right", as Go's filepath.Clean applies them:
///
///   1. a run of '/' becomes one '/';
///   2. a "." name is dropped;
///   3. a ".." name is dropped with the name before it;
///   4. a ".." right after the root of a rooted path is dropped: "/.." is "/";
///   5. a ".." that begins a relative path, or follows only other "..", is
///      kept, as there is nothing before it to drop;
///
/// and then a trailing '/' is gone, and what is left empty becomes ".". So
/// "/boot//bin/./x/../y/" is "/boot/bin/y", "" is ".", and "../a" stays
/// "../a".
///
/// Lexical, so ".." means the name before it in the text, not the directory a
/// server would call the parent. The two agree while there are no symbolic
/// links, and an ours.io server takes only clean paths: no "." or ".." names
/// (design D21).
///
/// BadPath when |path| is longer than MAX_PATH_LENGTH. |out| is then empty.
///
/// Whether the path named a directory (a trailing '/', or a last name of "."
/// or "..") is not kept: only a POSIX open() would turn that into
/// PROTOCOL_DIRECTORY, fsio has no POSIX layer, and its open() takes its
/// flags from the caller.
[[nodiscard]]
auto clean_path(std::string_view path, PathBuffer &out) -> ours::Status;
} // namespace fsio

#endif // #ifndef FSIO_PATH_HPP
