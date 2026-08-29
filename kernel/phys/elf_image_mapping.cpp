/// ElfImage::fix_mapping() only.
///
/// Split out of elf-image.cpp because it is the one ElfImage method that
/// touches Aspace -- calling aspace.map() needs Aspace::map()'s
/// implementation (aspace.cpp) linked in, which in turn needs BootMem
/// (global_bootmem()).  physload has both; boot-shim, which links
/// elf-image.cpp on its own to load physload, has neither and never calls
/// fix_mapping (it runs identity-mapped already).  Keeping this in its own
/// translation unit means boot-shim's build never has to satisfy those
/// symbols just because they live in the same file.

#include <ours/phys/aspace.hpp>
#include <ours/phys/elf_image.hpp>

#include <limits>
#include <type_traits>

namespace ours::phys {
namespace {
    template <typename Value>
    [[nodiscard]]
    constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    [[nodiscard]]
    constexpr auto map_permissions(eftl::elf::SegPerms permissions) noexcept -> MmuFlags {
        auto flags = MmuFlags::Present;
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Readable) != 0U) {
            flags |= MmuFlags::Readable;
        }
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Writable) != 0U) {
            flags |= MmuFlags::Writable;
        }
        if (static_cast<unsigned>(permissions & eftl::elf::SegPerms::Executable) != 0U) {
            flags |= MmuFlags::Executable;
        }
        return flags;
    }

    [[nodiscard]]
    constexpr auto page_count(usize byte_count) noexcept -> ustl::Option<usize> {
        if (byte_count == 0) {
            return ustl::NONE;
        }
        // Named PAGE_BYTES, not PAGE_SIZE: the latter is an object-like macro,
        // so the definition would expand into itself.
        constexpr auto PAGE_BYTES = static_cast<usize>(PAGE_SIZE);
        auto const whole_pages = byte_count / PAGE_BYTES;
        if (byte_count % PAGE_BYTES == 0) {
            return ustl::some(static_cast<usize>(whole_pages));
        }
        if (whole_pages == std::numeric_limits<usize>::max()) {
            return ustl::NONE;
        }
        return ustl::some(whole_pages + 1);
    }
} // namespace

// Map each ELF load interval independently.  Page-granularity mappings cover a
// segment's leading and trailing partial pages, while page-sized gaps remain unmapped.
auto ElfImage::fix_mapping(Aspace &aspace, VirtAddr target_base) -> Status {
    if (target_base % PAGE_SIZE != 0 || m_pa_load % PAGE_SIZE != 0) {
        return Status::InvalidArgument;
    }

    for (auto const &segment: m_load_plan.segments()) {
        auto const &region = segment.region;
        if (region.image_offset > size() || segment.memory_size > size() - region.image_offset ||
            target_base > std::numeric_limits<VirtAddr>::max() - region.image_offset ||
            m_pa_load > std::numeric_limits<PhysAddr>::max() - region.image_offset) {
            return Status::InvalidArgument;
        }
        auto const native_size = to_usize(segment.memory_size);
        if (native_size.is_none()) {
            return Status::InvalidArgument;
        }
        auto const virtual_address = target_base + region.image_offset;
        auto const physical_address = m_pa_load + region.image_offset;
        // Named PAGE_BYTES, not PAGE_SIZE: the latter is an object-like macro,
        // so the definition would expand into itself.
        constexpr auto PAGE_BYTES = static_cast<usize>(PAGE_SIZE);
        auto const page_offset = virtual_address % PAGE_BYTES;
        if (page_offset != physical_address % PAGE_BYTES ||
            native_size.value() > std::numeric_limits<usize>::max() - page_offset) {
            return Status::InvalidArgument;
        }
        auto const pages = page_count(native_size.value() + page_offset);
        if (pages.is_none()) {
            return Status::InvalidArgument;
        }

        auto const virtual_page = virtual_address - page_offset;
        auto const physical_page = physical_address - page_offset;
        if (pages.value() - 1 >
                (std::numeric_limits<VirtAddr>::max() - virtual_page) / PAGE_BYTES ||
            pages.value() - 1 >
                (std::numeric_limits<PhysAddr>::max() - physical_page) / PAGE_BYTES) {
            return Status::InvalidArgument;
        }
        auto const mapped = aspace.map(virtual_page, pages.value(), physical_page,
                                       map_permissions(region.permissions));
        if (mapped.is_err()) {
            return Status::InvalidArgument;
        }
    }
    return Status::Ok;
}

} // namespace ours::phys
