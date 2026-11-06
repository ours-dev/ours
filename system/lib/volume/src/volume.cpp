/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <volume/volume.hpp>

#include <algorithm>

#include <cstring>

namespace volume {
using ours::Status;

namespace {
    // The wire layout: the fixed fields' offsets, then the LV slots and the
    // segments, back to back, the CRC at the tail. u32 fields little-endian;
    // the names are byte arrays already.
    constexpr usize OFF_MAGIC = 0;
    constexpr usize OFF_VERSION = 8;
    constexpr usize OFF_UUID = 12;
    constexpr usize OFF_VG_UUID = 28;
    constexpr usize OFF_VG_NAME = 44;
    constexpr usize OFF_EXTENT_SECTORS = 60;
    constexpr usize OFF_PV_INDEX = 64;
    constexpr usize OFF_PV_EXTENTS = 68;
    constexpr usize OFF_LV_COUNT = 72;
    constexpr usize OFF_SEGMENT_COUNT = 76;
    constexpr usize OFF_LVS = 80;

    constexpr usize OFF_SEGMENTS = OFF_LVS + MAX_LVS * LV_SLOT_BYTES;

    constexpr usize OFF_CRC = HEADER_BYTES - 4;

    auto put32(std::span<u8> out, usize const at, u32 const value) -> void {
        for (auto i = usize{0}; i < 4; ++i) {
            out[at + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }

    auto get32(std::span<u8 const> const in, usize const at) -> u32 {
        u32 value = 0;
        for (auto i = usize{0}; i < 4; ++i) {
            value |= static_cast<u32>(in[at + i]) << (8 * i);
        }
        return value;
    }

    auto put64(std::span<u8> out, usize const at, u64 const value) -> void {
        for (auto i = usize{0}; i < 8; ++i) {
            out[at + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }

    auto get64(std::span<u8 const> const in, usize const at) -> u64 {
        u64 value = 0;
        for (auto i = usize{0}; i < 8; ++i) {
            value |= static_cast<u64>(in[at + i]) << (8 * i);
        }
        return value;
    }

    auto put_name(std::span<u8> out, usize const at, std::span<u8 const> const name) -> void {
        // NUL padded, and the field is MAX_NAME: a name that fills it has no
        // terminator, which the reader treats as full-width.
        std::memcpy(out.data() + at, name.data(), name.size());
    }

    auto copy_name(std::span<u8> name, std::span<u8 const> const field) -> void {
        std::memcpy(name.data(), field.data(), name.size());
    }

    /// Whether the LV geometry a header names adds up: a linear LV has no
    /// stripe fields, a striped one names its legs and a chunk, and every
    /// kind's segment accounting fits the slots. The segment extents
    /// themselves are the assembler's check (they live on other PVs).
    auto geometry_ok(std::span<Segment const> const segments, usize const segment_count) -> bool {
        for (auto const &segment: segments.first(segment_count)) {
            if (segment.role != static_cast<u16>(Kind::Linear) &&
                segment.role != static_cast<u16>(Kind::Striped)) {
                return false;
            }
        }
        return true;
    }
} // namespace

auto crc32(std::span<u8 const> const bytes) -> u32 {
    // zlib's CRC-32, the GPT's own: one table walks both, and the maker
    // script shares the constant.
    auto crc = 0xFFFF'FFFFu;
    for (auto const byte: bytes) {
        crc ^= byte;
        for (auto bit = u32{0}; bit < 8; ++bit) {
            auto const mask = static_cast<u32>(0u - (crc & 1));
            crc = (crc >> 1) ^ (0xEDB8'8320u & mask);
        }
    }
    return ~crc;
}

auto encode_header(std::span<u8> const out, Header const &header) -> ustl::Result<void, Status> {
    if (out.size() != HEADER_BYTES) {
        return ustl::err(Status::InvArgs);
    }
    if (header.lv_count > MAX_LVS || header.segment_count > MAX_SEGMENTS) {
        return ustl::err(Status::InvArgs);
    }
    for (auto const &lv: std::span{header.lvs}.first(header.lv_count)) {
        if (lv.kind == Kind::Striped && (lv.stripe_count == 0 || lv.chunk_sectors == 0)) {
            return ustl::err(Status::InvArgs);
        }
        if (lv.kind == Kind::Linear && (lv.stripe_count != 0)) {
            return ustl::err(Status::InvArgs);
        }
    }
    if (!geometry_ok(header.segments, header.segment_count)) {
        return ustl::err(Status::InvArgs);
    }
    for (auto const &segment: std::span{header.segments}.first(header.segment_count)) {
        if (segment.lv >= header.lv_count) {
            return ustl::err(Status::InvArgs);
        }
        if (segment.first_physical + u64{segment.extent_count} > header.pv_extent_count) {
            return ustl::err(Status::InvArgs);
        }
    }

    std::memset(out.data(), 0, out.size());
    std::memcpy(out.data() + OFF_MAGIC, MAGIC.data(), MAGIC.size());
    put32(out, OFF_VERSION, VERSION);
    std::memcpy(out.data() + OFF_UUID, header.uuid.data(), header.uuid.size());
    std::memcpy(out.data() + OFF_VG_UUID, header.vg_uuid.data(), header.vg_uuid.size());
    put_name(out, OFF_VG_NAME, header.vg_name);
    put32(out, OFF_EXTENT_SECTORS, header.extent_sectors);
    put32(out, OFF_PV_INDEX, header.pv_index);
    put32(out, OFF_PV_EXTENTS, header.pv_extent_count);
    put32(out, OFF_LV_COUNT, header.lv_count);
    put32(out, OFF_SEGMENT_COUNT, header.segment_count);
    for (auto index = usize{0}; index < header.lv_count; ++index) {
        auto const &lv = header.lvs[index];
        auto const at = OFF_LVS + index * LV_SLOT_BYTES;
        put_name(out, at, lv.name);
        put32(out, at + MAX_NAME, static_cast<u32>(lv.kind));
        put32(out, at + MAX_NAME + 4, lv.chunk_sectors);
        put32(out, at + MAX_NAME + 8, lv.stripe_count);
        put64(out, at + MAX_NAME + 12, lv.sector_count);
    }
    for (auto index = usize{0}; index < header.segment_count; ++index) {
        auto const &segment = header.segments[index];
        auto const at = OFF_SEGMENTS + index * SEGMENT_BYTES;
        put32(out, at, segment.lv | u32{segment.pv} << 16);
        put32(out, at + 4, segment.role | u32{segment.pad} << 16);
        put32(out, at + 8, segment.first_logical);
        put32(out, at + 12, segment.first_physical);
        put32(out, at + 16, segment.extent_count);
    }
    // The CRC over the whole block with its own field zero -- the memset
    // above left it zero -- and the stored field is excluded from what it
    // covers only by being written after: the decode's zeroed-tail form,
    // not a shorter range, is what must match.
    put32(out, OFF_CRC, crc32(out));
    return ustl::ok();
}

auto decode_header(std::span<u8 const> const in) -> ustl::Result<Header, Status> {
    if (in.size() != HEADER_BYTES) {
        return ustl::err(Status::InvArgs);
    }
    auto const crc = get32(in, OFF_CRC);
    auto zeroed = std::array<u8, HEADER_BYTES>{};
    std::memcpy(zeroed.data(), in.data(), in.size());
    std::memset(zeroed.data() + OFF_CRC, 0, 4);
    if (crc32(zeroed) != crc) {
        return ustl::err(Status::InvArgs);
    }
    if (std::memcmp(in.data() + OFF_MAGIC, MAGIC.data(), MAGIC.size()) != 0 ||
        get32(in, OFF_VERSION) != VERSION) {
        return ustl::err(Status::InvArgs);
    }
    auto header = Header{};
    std::memcpy(header.uuid.data(), in.data() + OFF_UUID, header.uuid.size());
    std::memcpy(header.vg_uuid.data(), in.data() + OFF_VG_UUID, header.vg_uuid.size());
    copy_name(header.vg_name, in.subspan(OFF_VG_NAME, MAX_NAME));
    header.extent_sectors = get32(in, OFF_EXTENT_SECTORS);
    header.pv_index = get32(in, OFF_PV_INDEX);
    header.pv_extent_count = get32(in, OFF_PV_EXTENTS);
    header.lv_count = get32(in, OFF_LV_COUNT);
    header.segment_count = get32(in, OFF_SEGMENT_COUNT);
    if (header.lv_count > MAX_LVS || header.segment_count > MAX_SEGMENTS) {
        return ustl::err(Status::InvArgs);
    }
    for (auto index = usize{0}; index < header.lv_count; ++index) {
        auto &lv = header.lvs[index];
        auto const at = OFF_LVS + index * LV_SLOT_BYTES;
        copy_name(lv.name, in.subspan(at, MAX_NAME));
        auto const kind = get32(in, at + MAX_NAME);
        if (kind != static_cast<u32>(Kind::Linear) && kind != static_cast<u32>(Kind::Striped)) {
            return ustl::err(Status::InvArgs);
        }
        lv.kind = static_cast<Kind>(kind);
        lv.chunk_sectors = get32(in, at + MAX_NAME + 4);
        lv.stripe_count = get32(in, at + MAX_NAME + 8);
        lv.sector_count = get64(in, at + MAX_NAME + 12);
        if (lv.kind == Kind::Striped && (lv.stripe_count == 0 || lv.chunk_sectors == 0)) {
            return ustl::err(Status::InvArgs);
        }
        if (lv.kind == Kind::Linear && lv.stripe_count != 0) {
            return ustl::err(Status::InvArgs);
        }
    }
    for (auto index = usize{0}; index < header.segment_count; ++index) {
        auto &segment = header.segments[index];
        auto const at = OFF_SEGMENTS + index * SEGMENT_BYTES;
        auto const first = get32(in, at);
        auto const second = get32(in, at + 4);
        segment.lv = static_cast<u16>(first & 0xFFFF);
        segment.pv = static_cast<u16>(first >> 16);
        segment.role = static_cast<u16>(second & 0xFFFF);
        if (segment.role != static_cast<u16>(Kind::Linear) &&
            segment.role != static_cast<u16>(Kind::Striped)) {
            return ustl::err(Status::InvArgs);
        }
        if (segment.lv >= header.lv_count) {
            return ustl::err(Status::InvArgs);
        }
        segment.first_logical = get32(in, at + 8);
        segment.first_physical = get32(in, at + 12);
        segment.extent_count = get32(in, at + 16);
        if (segment.first_physical + u64{segment.extent_count} > header.pv_extent_count) {
            return ustl::err(Status::InvArgs);
        }
    }
    // The roles are checked per segment above, which is strictly what
    // geometry_ok would repeat here.
    return ustl::ok(header);
}

auto VolumeGroup::add_pv(Header const &header) -> Status {
    if (m_pv_count == m_pvs.size()) {
        return Status::NoResource;
    }
    if (m_named) {
        if (header.vg_uuid != m_vg_uuid) {
            return Status::InvArgs;
        }
    } else {
        m_vg_uuid = header.vg_uuid;
        m_named = true;
        m_lv_count = header.lv_count;
        for (auto index = usize{0}; index < header.lv_count; ++index) {
            m_lvs[index] = header.lvs[index];
        }
    }
    for (auto index = usize{0}; index < m_pv_count; ++index) {
        if (m_pvs[index].header.pv_index == header.pv_index) {
            return Status::InvArgs;
        }
        if (m_pvs[index].header.uuid == header.uuid) {
            return Status::InvArgs;
        }
    }
    m_pvs[m_pv_count].header = header;
    m_pv_count += 1;
    // The group's segment table is the union of the PVs' own: each header
    // carries its segments, and the assembler gathers them.
    for (auto const &segment: std::span{header.segments}.first(header.segment_count)) {
        if (m_segment_count == m_segments.size()) {
            return Status::NoResource;
        }
        m_segments[m_segment_count] = segment;
        m_segment_count += 1;
    }
    return Status::Ok;
}

auto VolumeGroup::is_complete() const -> bool {
    if (!m_named) {
        return false;
    }
    // Every segment's PV joined.
    for (auto const &segment: std::span{m_segments}.first(m_segment_count)) {
        auto found = false;
        for (auto index = usize{0}; index < m_pv_count; ++index) {
            if (m_pvs[index].header.pv_index == segment.pv) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    // Every striped LV's legs are all in the table: a leg whose PV never
    // joined has no segment here at all, and nothing else says it is
    // missing.
    for (auto index = u32{0}; index < m_lv_count; ++index) {
        auto const &shape = m_lvs[index];
        if (shape.kind != Kind::Striped) {
            continue;
        }
        for (auto leg = u32{0}; leg < shape.stripe_count; ++leg) {
            auto found = false;
            for (auto const &segment: std::span{m_segments}.first(m_segment_count)) {
                if (segment.lv == index && segment.role == static_cast<u16>(Kind::Striped) &&
                    segment.first_logical == leg) {
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}

auto VolumeGroup::map(u16 const lv, u64 const sector) const -> ustl::Result<PvSector, Status> {
    if (!m_named || lv >= m_lv_count) {
        return ustl::err(Status::InvArgs);
    }
    auto const &shape = m_lvs[lv];
    if (sector >= shape.sector_count) {
        return ustl::err(Status::InvArgs);
    }

    if (shape.kind == Kind::Striped) {
        // The chunk round-robin: which leg owns the sector, and where on
        // the leg's concatenation of extents it lies.
        auto const global_chunk = sector / shape.chunk_sectors;
        auto const leg = global_chunk % shape.stripe_count;
        auto const on_leg = (global_chunk / shape.stripe_count) * shape.chunk_sectors +
                            (sector % shape.chunk_sectors);
        // The leg is the segment of this LV whose role is a stripe leg
        // and whose first_logical names the leg. on_leg is in sectors of
        // this PV's leg: the physical sector is the leg's first extent's
        // first sector plus on_leg.
        for (auto const &segment: std::span{m_segments}.first(m_segment_count)) {
            if (segment.lv != lv || segment.role != static_cast<u16>(Kind::Striped) ||
                segment.first_logical != leg) {
                continue;
            }
            auto const phys = segment.first_physical * u64{EXTENT_SECTORS} + on_leg;
            for (auto index = usize{0}; index < m_pv_count; ++index) {
                if (m_pvs[index].header.pv_index == segment.pv) {
                    return ustl::ok(PvSector{.pv = segment.pv, .sector = phys});
                }
            }
            return ustl::err(Status::NotFound);
        }
        return ustl::err(Status::NotFound);
    }

    // A linear piece: the logical sector's extent, and the segment that
    // carries it. Segments are walked in order; the first whose logical
    // range covers the sector answers.
    auto const logical_extent = static_cast<u32>(sector / EXTENT_SECTORS);
    auto const within = sector % EXTENT_SECTORS;
    for (auto const &segment: std::span{m_segments}.first(m_segment_count)) {
        if (segment.lv != lv || segment.role != static_cast<u16>(Kind::Linear)) {
            continue;
        }
        if (logical_extent < segment.first_logical ||
            logical_extent >= segment.first_logical + segment.extent_count) {
            continue;
        }
        auto const physical = (segment.first_physical + (logical_extent - segment.first_logical)) *
                                  u64{EXTENT_SECTORS} +
                              within;
        for (auto index = usize{0}; index < m_pv_count; ++index) {
            if (m_pvs[index].header.pv_index == segment.pv) {
                return ustl::ok(PvSector{.pv = segment.pv, .sector = physical});
            }
        }
        return ustl::err(Status::NotFound);
    }
    return ustl::err(Status::NotFound);
}
} // namespace volume
