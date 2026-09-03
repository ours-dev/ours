#include "legacy_boot.hpp"
#include <logz4/log.hpp>
#include <ours/phys/init.hpp>

#include <cstring>
namespace ours::phys {
namespace {
    auto parse_string(multiboot::View::Tag const &tag, ustl::views::StringView &output) -> Status {
        auto const bytes = tag.bytes();
        if (bytes.size() <= sizeof(multiboot::MultibootTag)) {
            return Status::InvalidArgument;
        }
        auto const *text =
            reinterpret_cast<char const *>(bytes.data() + sizeof(multiboot::MultibootTag));
        auto const text_size = bytes.size() - sizeof(multiboot::MultibootTag);
        if (std::memchr(text, 0, text_size) == nullptr) {
            return Status::InvalidArgument;
        }
        output = text;
        return Status::Ok;
    }

    auto parse_acpi(multiboot::View::Tag const &tag, usize const minimum_size, usize const version,
                    usize &acpi_version, usize &acpi_rsdp) -> Status {
        if (tag.bytes().size() < minimum_size) {
            return Status::InvalidArgument;
        }
        acpi_version = version;
        acpi_rsdp = reinterpret_cast<usize>(tag.bytes().data() + sizeof(multiboot::MultibootTag));
        log::debug("ACPI-V{}, RSDP={}", version, acpi_rsdp);
        return Status::Ok;
    }
} // namespace

auto LegacyBoot::parse_params(usize const param) -> Status {
    name = {};
    cmdline = {};
    ramdisk = {};
    mbi_size = 0;
    memory_map = ustl::NONE;
    acpi_version = 0;
    acpi_rsdp = 0;
    if (param == 0) {
        return Status::InvalidArgument;
    }

    auto const *data = reinterpret_cast<u8 const *>(param);
    u32 total_size{};
    std::memcpy(&total_size, data, sizeof(total_size));
    mbi_size = total_size;
    auto const view = multiboot::View::from(data, total_size);
    if (view.is_err()) {
        return view.unwrap_err();
    }

    auto const module = view.unwrap().module();
    if (module.is_err()) {
        return module.unwrap_err();
    }
    auto const parsed_memory_map = view.unwrap().memory_map();
    if (parsed_memory_map.is_err()) {
        return parsed_memory_map.unwrap_err();
    }
    memory_map = parsed_memory_map.unwrap();
    ramdisk = {
        reinterpret_cast<u8 *>(static_cast<usize>(module.unwrap().start)),
        static_cast<usize>(module.unwrap().end - module.unwrap().start),
    };
    log::info("`ours.obi` is at 0x{:X}, size {} bytes", module.unwrap().start, ramdisk.size());

    auto tags = view.unwrap().iterator();
    while (auto const tag = tags.next()) {
        Status status = Status::Ok;
        switch (tag->type()) {
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
                status = parse_string(*tag, name);
                break;
            case MULTIBOOT_TAG_TYPE_CMDLINE:
                status = parse_string(*tag, cmdline);
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_OLD:
                status = parse_acpi(*tag, sizeof(multiboot::MultibootTag) + 20, 1, acpi_version,
                                    acpi_rsdp);
                break;
            case MULTIBOOT_TAG_TYPE_ACPI_NEW:
                status = parse_acpi(*tag, sizeof(multiboot::MultibootTag) + 36, 2, acpi_version,
                                    acpi_rsdp);
                break;
            default:
                break;
        }
        if (status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}
} // namespace ours::phys
