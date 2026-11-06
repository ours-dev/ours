/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef VOLUME_VOLUME_HPP
#define VOLUME_VOLUME_HPP 1

#include <array>
#include <span>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// The volume group's on-disk metadata and the extent mapping: how logical
/// volumes address physical ones, linear and striped. The model follows
/// LVM's: a physical volume is divided into extents (1 MiB here); a
/// logical volume is not a place data lives but a mapping -- segments
/// naming (pv, first physical extent, count) -- and every read and write
/// on the LV goes through it. A linear LV concatenates its segments; a
/// striped LV round-robins chunks over its legs, one segment per leg.
///
/// The metadata is the PV header: one binary block at the partition's
/// first sectors, double written (primary at LBA 0, secondary at LBA
/// HEADER_SECTORS), CRC'd like the GPT's. Each PV carries its own
/// segments; the consumer reads every PV's header, groups them by VG
/// uuid, and assembles the group. Bytes alone, no system call: the codec
/// and the mapping build in every toolchain, the host one included,
/// where their tests run.
namespace volume {

/// One sector, and one extent: 1 MiB of sectors, the unit the mapping
/// hands out. The header's copies live at LBA 0 and LBA HEADER_SECTORS;
/// data extents start at LBA EXTENT_SECTORS, the first extent boundary --
/// the sectors between are padding, and the extent arithmetic stays free
/// of the header's size.
inline constexpr u32 SECTOR_BYTES = 512;
inline constexpr u32 EXTENT_SECTORS = 2048;
inline constexpr u32 HEADER_SECTORS = 2;
inline constexpr u32 HEADER_BYTES = HEADER_SECTORS * SECTOR_BYTES;

/// The wire sizes of the header's two tables: an LV slot is the name,
/// the kind, the chunk, the leg count and the sector count -- 16 + 12 +
/// 8, padded to 40; a segment is four u16 and three u32, 20 bytes. The
/// structs above are the codec's view; these are the wire's.
inline constexpr usize LV_SLOT_BYTES = 40;
inline constexpr usize SEGMENT_BYTES = 20;

/// The header's magic and the one version this codec reads.
inline constexpr std::array<u8, 8> MAGIC = {'O', 'U', 'R', 'S', 'P', 'V', 0, 0};
inline constexpr u32 VERSION = 1;

/// How many LVs one VG names, and how many segments its metadata carries.
/// The slots are fixed so the header's size is: the reader's offsets never
/// move under a writer that counts differently.
inline constexpr usize MAX_LVS = 4;
inline constexpr usize MAX_SEGMENTS = 32;

/// The longest LV or VG name, NUL padded.
inline constexpr usize MAX_NAME = 16;

/// An LV's two kinds.
enum class Kind : u32 {
    Linear = 0,
    Striped = 1,
};

/// One LV slot of the header: what the mapping needs to route a sector.
/// A striped LV's legs live on other PVs; the stripe geometry (leg count,
/// chunk) is in every leg's header, because every leg answers GetInfo.
struct Lv {
    std::array<u8, MAX_NAME> name{};
    Kind kind = Kind::Linear;
    /// The stripe's interleave, in sectors. Linear LVs leave it zero.
    u32 chunk_sectors = 0;
    /// The legs a striped LV round-robins over. Linear: zero.
    u32 stripe_count = 0;
    /// The LV's size, in sectors.
    u64 sector_count = 0;
    /// The segments of this LV across the whole VG, all headers together.
    u32 segment_count = 0;
};

/// One segment: a run of extents on one PV, what the mapping translates
/// through. A linear piece's logical extents start at |first_logical|;
/// a stripe leg names its leg index there instead.
struct Segment {
    u16 lv = 0;
    u16 pv = 0;
    /// 0 linear piece, 1 stripe leg -- as Kind, in u16 storage.
    u16 role = 0;
    u16 pad = 0;
    u32 first_logical = 0;
    u32 first_physical = 0;
    u32 extent_count = 0;
};

/// The PV header, as the codec's struct: the fixed fields, the LV slots,
/// the segments. 1024 bytes on disk, little-endian, the CRC over the
/// whole block with its own field zeroed, at the tail -- the GPT's rule.
struct Header {
    std::array<u8, 16> uuid{};
    std::array<u8, 16> vg_uuid{};
    std::array<u8, MAX_NAME> vg_name{};
    u32 extent_sectors = EXTENT_SECTORS;
    u32 pv_index = 0;
    u32 pv_extent_count = 0;
    u32 lv_count = 0;
    u32 segment_count = 0;
    std::array<Lv, MAX_LVS> lvs{};
    std::array<Segment, MAX_SEGMENTS> segments{};
};

/// The CRC-32 the GPT uses (zlib's), so one table walks both.
[[nodiscard]]
auto crc32(std::span<u8 const> bytes) -> u32;

/// Encodes |header| into |out| (HEADER_BYTES): the fields little-endian,
/// the CRC at the tail. The names are stored NUL padded; a longer one, a
/// segment naming an LV past lv_count, a segment past its PV's extents,
/// or a striped LV whose geometry does not add up (legs whose extents do
/// not match the stripe's shape) is refused with InvArgs.
[[nodiscard]]
auto encode_header(std::span<u8> out, Header const &header) -> ustl::Result<void, ours::Status>;

/// Decodes a header block: the CRC first, then the fields. Refused with
/// InvArgs: bad magic, version, size, CRC, counts past the slots, or a
/// striped LV whose geometry does not add up.
[[nodiscard]]
auto decode_header(std::span<u8 const> in) -> ustl::Result<Header, ours::Status>;

/// Where a sector of an LV lives: the PV's index and the sector on it.
struct PvSector {
    u32 pv = 0;
    u64 sector = 0;
};

/// One PV of the group, as the assembler holds it: the header the PV's
/// own sectors answered with.
struct PvEntry {
    Header header{};
};

/// The assembled group: the PVs' headers collected, the LVs' segments
/// gathered, and the one function everything above goes through -- which
/// PV, which sector. The group is read-only once assembled: the mapping
/// is metadata, and metadata changes are reassembly.
class VolumeGroup {
  public:
    VolumeGroup() = default;

    /// Adds one PV's header. The first header names the group (its VG
    /// uuid); a header whose VG uuid differs is refused with InvArgs, as
    /// is a second header of one pv_index. The group is complete when
    /// every segment's pv has a header and every striped LV has all its
    /// legs; is_complete() says which.
    auto add_pv(Header const &header) -> ours::Status;

    /// Whether the group can serve its LVs: every segment's PV present,
    /// every striped LV's legs all there.
    [[nodiscard]]
    auto is_complete() const -> bool;

    /// How many PVs joined.
    [[nodiscard]]
    auto pv_count() const -> usize {
        return m_pv_count;
    }

    /// How many segments the group's table carries.
    [[nodiscard]]
    auto segment_total() const -> usize {
        return m_segment_count;
    }

    /// The LV slots, by index: the geometry the LV's own GetInfo answers
    /// with.
    [[nodiscard]]
    auto lv(u16 const index) const -> Lv const & {
        return m_lvs[index];
    }

    /// How many LVs the group names.
    [[nodiscard]]
    auto lv_count() const -> u32 {
        return m_lv_count;
    }

    /// The mapping, the one function everything above goes through: which
    /// PV, which sector. InvArgs past the LV's end; NotFound for an
    /// incomplete group's missing piece.
    [[nodiscard]]
    auto map(u16 const lv, u64 const sector) const -> ustl::Result<PvSector, ours::Status>;

  private:
    std::array<PvEntry, 8> m_pvs{};
    usize m_pv_count = 0;
    std::array<Lv, MAX_LVS> m_lvs{};
    u32 m_lv_count = 0;
    std::array<Segment, MAX_SEGMENTS> m_segments{};
    usize m_segment_count = 0;
    std::array<u8, 16> m_vg_uuid{};
    bool m_named = false;
};
} // namespace volume

#endif // #ifndef VOLUME_VOLUME_HPP
