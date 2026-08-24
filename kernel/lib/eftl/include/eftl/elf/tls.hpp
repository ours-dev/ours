// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_TLS_HPP
#define EFTL_ELF_TLS_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace eftl::elf {
/// A borrowed ELF PT_TLS initialization template.
template <typename Traits>
class TlsTemplate final {
  public:
    using Segment = typename Traits::Segment;

    [[nodiscard]]
    static auto from(Segment const &segment, ByteRange initialized) noexcept
        -> ustl::Option<TlsTemplate> {
        auto const file_size = static_cast<u64>(segment.p_filesz);
        auto const memory_size = static_cast<u64>(segment.p_memsz);
        auto const virtual_start = static_cast<u64>(segment.p_vaddr);
        auto const alignment = static_cast<u64>(segment.p_align);
        auto const file_offset = static_cast<u64>(segment.p_offset);

        if (segment.type() != SegType::Tls ||
            (!initialized.empty() && initialized.data() == nullptr) ||
            file_size != static_cast<u64>(initialized.size()) || file_size > memory_size ||
            memory_size > std::numeric_limits<u64>::max() - virtual_start ||
            !has_valid_alignment(file_offset, virtual_start, alignment)) {
            return ustl::NONE;
        }
        return ustl::some(
            TlsTemplate(initialized, virtual_start, memory_size, alignment, segment.permissions()));
    }

    /// Returns the file-backed bytes copied before the zero-filled TLS tail.
    [[nodiscard]]
    constexpr auto initialized() const noexcept -> ByteRange {
        return m_initialized;
    }

    [[nodiscard]]
    constexpr auto virtual_start() const noexcept -> u64 {
        return m_virtual_start;
    }

    [[nodiscard]]
    constexpr auto memory_size() const noexcept -> u64 {
        return m_memory_size;
    }

    [[nodiscard]]
    constexpr auto alignment() const noexcept -> u64 {
        return m_alignment;
    }

    [[nodiscard]]
    constexpr auto permissions() const noexcept -> SegPerms {
        return m_permissions;
    }

  private:
    constexpr TlsTemplate(ByteRange initialized, u64 virtual_start, u64 memory_size,
                          u64 alignment, SegPerms permissions) noexcept
        : m_initialized(initialized)
        , m_virtual_start(virtual_start)
        , m_memory_size(memory_size)
        , m_alignment(alignment)
        , m_permissions(permissions) {}

    [[nodiscard]]
    static constexpr auto has_valid_alignment(u64 file_offset, u64 virtual_start,
                                              u64 alignment) noexcept -> bool {
        if (alignment <= 1) {
            return true;
        }
        return (alignment & (alignment - 1)) == 0 &&
               (file_offset & (alignment - 1)) == (virtual_start & (alignment - 1));
    }

    ByteRange m_initialized{};
    u64 m_virtual_start{0};
    u64 m_memory_size{0};
    u64 m_alignment{0};
    SegPerms m_permissions{SegPerms::None};
};
} // namespace eftl::elf

#endif // EFTL_ELF_TLS_HPP
