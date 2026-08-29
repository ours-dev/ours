/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <unwind/cursor.hpp>

#include <unwind/cfi.hpp>

namespace unwind {
namespace {
    constexpr std::size_t ULEB_MAX_SHIFT = 63;
} // namespace

auto Cursor::checked_slice(std::size_t count) noexcept -> std::uint8_t const * {
    if (!m_ok) {
        return nullptr;
    }
    if (count > m_size - m_pos) {
        m_ok = false;
        return nullptr;
    }

    std::uint8_t const *result = m_data + m_pos;
    m_pos += count;
    return result;
}

auto Cursor::u8() noexcept -> std::uint8_t {
    std::uint8_t const *slice = checked_slice(1);
    if (slice == nullptr) {
        return 0;
    }
    return slice[0];
}

auto Cursor::u16() noexcept -> std::uint16_t {
    std::uint8_t const *slice = checked_slice(2);
    if (slice == nullptr) {
        return 0;
    }
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(slice[0]) |
                                      (static_cast<std::uint16_t>(slice[1]) << 8));
}

auto Cursor::u32() noexcept -> std::uint32_t {
    std::uint8_t const *slice = checked_slice(4);
    if (slice == nullptr) {
        return 0;
    }
    return static_cast<std::uint32_t>(slice[0]) | (static_cast<std::uint32_t>(slice[1]) << 8) |
           (static_cast<std::uint32_t>(slice[2]) << 16) | (static_cast<std::uint32_t>(slice[3]) << 24);
}

auto Cursor::u64() noexcept -> std::uint64_t {
    std::uint8_t const *slice = checked_slice(8);
    if (slice == nullptr) {
        return 0;
    }
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        result |= static_cast<std::uint64_t>(slice[i]) << (8 * i);
    }
    return result;
}

auto Cursor::uleb() noexcept -> std::uint64_t {
    std::uint64_t result = 0;
    std::size_t shift = 0;
    while (m_ok) {
        std::uint8_t const byte = u8();
        if (!m_ok) {
            return 0;
        }
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return result;
        }
        if (shift >= ULEB_MAX_SHIFT) {
            m_ok = false;
            return 0;
        }
        shift += 7;
    }
    return 0;
}

auto Cursor::sleb() noexcept -> std::int64_t {
    std::uint64_t result = 0;
    std::size_t shift = 0;
    std::uint8_t byte = 0;
    while (m_ok) {
        byte = u8();
        if (!m_ok) {
            return 0;
        }
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
        if (shift >= ULEB_MAX_SHIFT) {
            m_ok = false;
            return 0;
        }
    }
    if ((byte & 0x40) != 0 && shift < 64) {
        result |= (~std::uint64_t{0}) << shift;
    }
    return static_cast<std::int64_t>(result);
}

auto Cursor::skip(std::size_t count) noexcept -> void {
    checked_slice(count);
}

auto Cursor::read_bytes(std::uint8_t *destination, std::size_t count) noexcept -> void {
    std::uint8_t const *slice = checked_slice(count);
    if (slice == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] = slice[i];
    }
}

auto Cursor::encoded_pointer(std::uint8_t encoding, std::uint64_t pc_base) noexcept -> std::uint64_t {
    if (encoding == DW_EH_PE_OMIT) {
        m_ok = false;
        return 0;
    }
    if ((encoding & DW_EH_PE_INDIRECT) != 0) {
        m_ok = false;
        return 0;
    }

    std::uint64_t value = 0;
    switch (encoding & DW_EH_PE_FORMAT_MASK) {
        case DW_EH_PE_ABSPTR:
            value = u64();
            break;
        case DW_EH_PE_ULEB128:
            value = uleb();
            break;
        case DW_EH_PE_UDATA2:
            value = u16();
            break;
        case DW_EH_PE_UDATA4:
            value = u32();
            break;
        case DW_EH_PE_UDATA8:
            value = u64();
            break;
        case DW_EH_PE_SLEB128:
            value = static_cast<std::uint64_t>(sleb());
            break;
        case DW_EH_PE_SDATA2:
            value = static_cast<std::uint64_t>(static_cast<std::int16_t>(u16()));
            break;
        case DW_EH_PE_SDATA4:
            value = static_cast<std::uint64_t>(static_cast<std::int32_t>(u32()));
            break;
        case DW_EH_PE_SDATA8:
            value = static_cast<std::uint64_t>(static_cast<std::int64_t>(u64()));
            break;
        default:
            m_ok = false;
            return 0;
    }

    switch (encoding & DW_EH_PE_APPLICATION_MASK) {
        case 0:
            break;
        case DW_EH_PE_PCREL:
            value += pc_base;
            break;
        default:
            // TEXTREL / DATAREL / FUNCREL / ALIGNED cannot be resolved from
            // the section contents alone.
            m_ok = false;
            return 0;
    }

    if (!m_ok) {
        return 0;
    }
    return value;
}

} // namespace unwind
