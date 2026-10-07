/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef CRASHLOG_CRASHLOG_HPP
#define CRASHLOG_CRASHLOG_HPP 1

#include <span>
#include <string_view>

#include <cstddef>

#include <ours/types.hpp>
#include <ustl/option.hpp>

/// The panic crash record: one CRC-checked record in a region of RAM that a
/// warm reset leaves alone, written by a panic and read by the next boot.
///
/// This library is only the record's format. It knows nothing of where the
/// region is or how it is mapped -- the kernel finds it in the memory map
/// (bootmem::MemoryType::Crashlog) and passes it in as bytes -- so the format
/// is tested on the host.
///
/// One header copy is enough: the record is written once, by a panic, and
/// never updated, so there is no in-place update that a second copy would
/// have to make atomic.
namespace crashlog {

/// The start of the region. Every field is native-endian: the reader is the
/// same kernel on the same machine, one boot later.
struct Header {
    /// MAGIC when the record is whole, anything else when there is none.
    /// Written last; see Writer::seal().
    u64 magic;
    u32 version;
    u32 payload_len;
    u32 payload_crc32;

    /// The CRC of this header, computed with `magic` and this field taken as
    /// zero, so that the CRC can be written before the magic.
    u32 header_crc32;

    /// When the record was written, in the writer's clock ticks.
    u64 time;
};
static_assert(sizeof(Header) == 32);

inline constexpr auto MAGIC = u64{0x6b6f6c6873617263}; // "crashlok", little-endian
inline constexpr auto VERSION = u32{1};

/// Builds one record in `region`, which must be at least sizeof(Header)
/// bytes and 8-byte aligned.
///
/// Construction invalidates whatever record the region held, before a byte
/// of the new one is written: from then until seal(), a reset leaves a
/// region with no record rather than a half-written one passed off as the
/// old one.
class Writer final {
  public:
    explicit Writer(std::span<std::byte> region);

    /// Appends `text` to the payload. Text past the region's end is dropped
    /// and remembered; truncated() says so. A record cut short is still a
    /// record: the start of a panic report is the part that matters.
    auto append(std::string_view text) -> void;

    /// Writes the header: the lengths and CRCs first, then the magic. The
    /// magic is the commit -- a reset at any point before it lands leaves no
    /// record, and a reset after it leaves a whole one. Making the bytes reach
    /// RAM before a reset (writing the cache back) is the caller's job.
    auto seal(u64 time) -> void;

    [[nodiscard]]
    auto truncated() const -> bool {
        return m_truncated;
    }

  private:
    std::span<std::byte> m_region;
    usize m_length = 0;
    /// Whether append() dropped text. The writer's one yes/no fact, so it has
    /// no word to share, and m_length cannot say it: a payload that exactly
    /// fills the region dropped nothing.
    bool m_truncated = false;
};

/// A whole record, as read back.
struct Record {
    u64 time;
    std::string_view text;
};

/// The record in `region`, if it holds a whole one: the right magic and
/// version, a payload that fits, and both CRCs matching. Anything else --
/// a region never written, written by a different version, cut off by a
/// reset, or decayed while the power was off -- is no record. The text
/// points into `region`.
[[nodiscard]]
auto read(std::span<std::byte const> region) -> ustl::Option<Record>;

/// Invalidates the record in `region`, so that the next read() finds none.
/// The boot that reports a record clears it, so it is reported once.
auto clear(std::span<std::byte> region) -> void;

} // namespace crashlog

#endif // #ifndef CRASHLOG_CRASHLOG_HPP
