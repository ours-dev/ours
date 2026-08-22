#include <ours/phys/kernel-image.hpp>

#include <limits>

#include <cstring>

namespace ours::phys {
auto RawImage::init(ustl::views::Span<u8 const> file, usize memory_size) -> Status {
    if (memory_size < file.size()) {
        return Status::InvalidArgument;
    }
    m_file = file;
    m_memory_size = memory_size;
    return Status::Ok;
}

auto RawImage::load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> {
    if (phys_addr > std::numeric_limits<PhysAddr>::max() - m_memory_size) {
        return ustl::err(Status::OutOfRange);
    }
    if (!m_file.empty()) {
        std::memcpy(reinterpret_cast<u8 *>(phys_addr), m_file.data(), m_file.size());
    }
    if (m_memory_size > m_file.size()) {
        std::memset(reinterpret_cast<u8 *>(phys_addr) + m_file.size(), 0,
                    m_memory_size - m_file.size());
    }
    m_pa_load = phys_addr;
    return ustl::ok(phys_addr);
}

auto RawImage::fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status {
    constexpr auto K_PAGE_SIZE = static_cast<usize>(PAGE_SIZE);
    if (m_memory_size == 0 || target_base % K_PAGE_SIZE != 0 || m_pa_load % K_PAGE_SIZE != 0) {
        return Status::InvalidArgument;
    }

    auto const whole_pages = m_memory_size / K_PAGE_SIZE;
    auto const page_count = whole_pages + (m_memory_size % K_PAGE_SIZE == 0 ? 0 : 1);
    if (page_count == 0 ||
        page_count - 1 > (std::numeric_limits<VirtAddr>::max() - target_base) / K_PAGE_SIZE ||
        page_count - 1 > (std::numeric_limits<PhysAddr>::max() - m_pa_load) / K_PAGE_SIZE) {
        return Status::InvalidArgument;
    }

    auto const mapped =
        aspace.map(target_base, page_count, m_pa_load, MmuFlags::PermMask | MmuFlags::Present);
    return mapped.is_ok() ? Status::Ok : Status::InvalidArgument;
}
} // namespace ours::phys
