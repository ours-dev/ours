#include <ours/phys/kernel-image.hpp>

#include <limits>
#include <type_traits>

#include <cstring>

#include <eftl/elf/mapped_memory.hpp>

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

    template <typename Traits>
    [[nodiscard]]
    auto dynamic_entries(eftl::elf::FileImpl<Traits> const &binary)
        -> ustl::Option<eftl::elf::TableView<typename Traits::Dyn>> {
        auto const image = binary.dynamic_image();
        return image.is_some() ? ustl::some(image->entries()) : ustl::NONE;
    }

    template <typename Traits, typename IsRelative>
    [[nodiscard]]
    auto apply_kernel_relative_relocations(eftl::elf::FileImpl<Traits> const &binary,
                                           eftl::elf::MappedMemory<Traits> &memory,
                                           typename Traits::Addr bias, IsRelative &&is_relative)
        -> bool {
        auto const dynamics = dynamic_entries(binary);
        if (dynamics.is_none()) {
            return false;
        }
        if (dynamics->empty()) {
            return true;
        }

        eftl::elf::Relocations<Traits> relocations;
        if (!eftl::elf::decode_dynamic(memory, dynamics.value(),
                                       eftl::elf::RelocationObserver<Traits>(relocations))) {
            return false;
        }
        return eftl::elf::relocate_relative(memory, relocations, bias,
                                            std::forward<IsRelative>(is_relative));
    }

    [[nodiscard]]
    auto relative_relocation_type(eftl::elf::Arch machine, u32 type) -> bool {
        switch (machine) {
            case eftl::elf::Arch::X86_64:
                return type == 8; // R_X86_64_RELATIVE
            case eftl::elf::Arch::AARCH64:
                return type == 1027; // R_AARCH64_RELATIVE
            case eftl::elf::Arch::RISCV:
                return type == 3; // R_RISCV_RELATIVE
            default:
                return false;
        }
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
        constexpr auto K_PAGE_SIZE = static_cast<usize>(PAGE_SIZE);
        auto const whole_pages = byte_count / K_PAGE_SIZE;
        if (byte_count % K_PAGE_SIZE == 0) {
            return ustl::some(static_cast<usize>(whole_pages));
        }
        if (whole_pages == std::numeric_limits<usize>::max()) {
            return ustl::NONE;
        }
        return ustl::some(whole_pages + 1);
    }
} // namespace

auto ElfImage::init(ustl::views::Span<u8 const> file, usize memory_size) -> Status {
    m_binary = ustl::NONE;
    m_load_plan = LoadPlan{};
    auto parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        return Status::InvalidArgument;
    }

    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return Status::InvalidArgument;
    }
    auto planned = LoadPolicy::make_plan(native_image.value());
    if (planned.is_err() || memory_size < planned->image_size()) {
        return Status::InvalidArgument;
    }

    m_binary = ustl::some(std::move(parsed).unwrap());
    m_load_plan = std::move(planned).unwrap();
    return Status::Ok;
}

auto ElfImage::load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> {
    if (phys_addr > std::numeric_limits<PhysAddr>::max() - size()) {
        return ustl::err(Status::OutOfRange);
    }

    if (size() != 0) {
        std::memset(reinterpret_cast<u8 *>(phys_addr), 0, size());
    }
    for (auto const &segment: m_load_plan.segments()) {
        auto const &region = segment.region;
        if (region.image_offset > size() || segment.memory_size > size() - region.image_offset ||
            segment.file_size > segment.memory_size ||
            segment.content.size() != segment.file_size) {
            return ustl::err(Status::InvalidArgument);
        }
        if (phys_addr > std::numeric_limits<PhysAddr>::max() - region.image_offset) {
            return ustl::err(Status::OutOfRange);
        }

        auto const alignment = to_usize(segment.alignment);
        if (alignment.is_none()) {
            return ustl::err(Status::InvalidArgument);
        }
        auto const destination_address = phys_addr + region.image_offset;
        if (alignment.value() > 1 && (destination_address % alignment.value()) !=
                                         (region.virtual_start % alignment.value())) {
            return ustl::err(Status::InvalidArgument);
        }

        auto *const destination = reinterpret_cast<u8 *>(destination_address);
        if (!segment.content.empty()) {
            std::memcpy(destination, segment.content.data(), segment.content.size());
        }
        auto const zero_size = static_cast<usize>(segment.memory_size - segment.file_size);
        if (zero_size != 0) {
            std::memset(destination + segment.content.size(), 0, zero_size);
        }
    }

    auto const entry_offset = m_load_plan.entry_offset();
    if (entry_offset.is_none() ||
        phys_addr > std::numeric_limits<PhysAddr>::max() - entry_offset.value()) {
        return ustl::err(Status::OutOfRange);
    }
    m_pa_load = phys_addr;
    return ustl::ok(m_pa_load + entry_offset.value());
}

auto ElfImage::relocate(usize load_address) -> void {
    if (m_binary.is_none() || load_address < m_load_plan.image_base()) {
        panic("Kernel load address precedes the ELF virtual base");
    }
    if constexpr (sizeof(usize) > sizeof(Traits::Addr)) {
        if (load_address > std::numeric_limits<typename Traits::Addr>::max()) {
            panic("Kernel load address does not fit the ELF address width");
        }
    }

    eftl::elf::MappedMemory<Traits> image(reinterpret_cast<u8 *>(m_pa_load), size(),
                                          m_load_plan.regions());
    auto const load_bias =
        static_cast<typename Traits::Addr>(load_address - m_load_plan.image_base());
    auto const machine = static_cast<eftl::elf::Arch>(m_binary.value().elf_header().e_machine);
    if (!apply_kernel_relative_relocations(m_binary.value(), image, load_bias, [machine](u32 type) {
            return relative_relocation_type(machine, type);
        })) {
        panic("Failed to relocate kernel ELF image");
    }

    m_va_load = load_address;
}

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
        constexpr auto K_PAGE_SIZE = static_cast<usize>(PAGE_SIZE);
        auto const page_offset = virtual_address % K_PAGE_SIZE;
        if (page_offset != physical_address % K_PAGE_SIZE ||
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
                (std::numeric_limits<VirtAddr>::max() - virtual_page) / K_PAGE_SIZE ||
            pages.value() - 1 >
                (std::numeric_limits<PhysAddr>::max() - physical_page) / K_PAGE_SIZE) {
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
