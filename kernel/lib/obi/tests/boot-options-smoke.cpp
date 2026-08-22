#include <obi/command-line-view.hpp>
#include <obi/container-view.hpp>
#include <obi/package-builder.hpp>
#include <ours/phys/boot-options.hpp>

#include <array>
#include <string>
#include <string_view>

#include <cstdlib>

namespace {
using obi::Compression;
using obi::ContainerView;
using obi::ItemFlags;
using obi::ItemFormat;
using obi::ItemSource;
using obi::ItemType;
using obi::PackageBuilder;

auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}

auto command_line_from(obi::ItemView::Bytes payload) -> obi::CommandLineView {
    PackageBuilder builder;
    auto const added = builder.add_item(ItemSource{
        .name = "cmdline",
        .type = ItemType::Config,
        .format = ItemFormat::Raw,
        .data = payload,
        .flags = ItemFlags::Required,
        .memory_size = payload.size(),
        .alignment = 8,
        .compression = Compression::None,
    });
    require(added.is_ok());

    static auto container = std::vector<u8>{};
    container = builder.build(/*crc=*/false);
    require(!container.empty());
    auto package = ContainerView::from(container.data(), container.size());
    require(package.is_ok());
    auto item = package.unwrap().find("cmdline");
    require(item.is_ok() && item.unwrap().is_some());
    auto command_line = obi::CommandLineView::from(item.unwrap().value());
    require(command_line.is_ok());
    return command_line.unwrap();
}

/// String literals have static storage, so the span handed to
/// command_line_from() outlives the builder inside it.
auto options_from_literal(std::string_view text) -> obi::CommandLineView {
    return command_line_from(
        obi::ItemView::Bytes(reinterpret_cast<u8 const *>(text.data()), text.size()));
}
} // namespace

auto main() -> int {
    constexpr auto options_text = std::array<u8, 17>{
        'a', 's', 'l', 'r', '.', 'd', 'i', 's', 'a', 'b', 'l', 'e', 'd', '=', '0', '\n', 0,
    };
    auto const options =
        command_line_from(obi::ItemView::Bytes(options_text.data(), options_text.size()));
    auto boot_options = ours::phys::BootOptions{};
    require(boot_options.aslr_disabled);
    require(ours::phys::apply_boot_options(boot_options, options) == ours::Status::Ok);
    require(!boot_options.aslr_disabled);

    constexpr auto bare_flag = std::array<u8, 14>{
        'a', 's', 'l', 'r', '.', 'd', 'i', 's', 'a', 'b', 'l', 'e', 'd', '\n',
    };
    auto bare_options = ours::phys::BootOptions{};
    require(ours::phys::apply_boot_options(
                bare_options, command_line_from(obi::ItemView::Bytes(
                                  bare_flag.data(), bare_flag.size()))) == ours::Status::Ok);
    require(bare_options.aslr_disabled);

    constexpr auto invalid_bool = std::array<u8, 19>{
        'a', 's', 'l', 'r', '.', 'd', 'i', 's', 'a', 'b',
        'l', 'e', 'd', '=', 'm', 'a', 'y', 'b', 'e',
    };
    auto invalid_options = ours::phys::BootOptions{};
    require(ours::phys::apply_boot_options(
                invalid_options, command_line_from(obi::ItemView::Bytes(invalid_bool.data(),
                                                                        invalid_bool.size()))) ==
            ours::Status::InvalidArgument);

    // The module/kernel lookups keep their defaults unless the command line
    // overrides them (kernel.phys.next=physboot is equivalent to omitting it).
    auto named_options = ours::phys::BootOptions{};
    require(std::string_view(boot_options.next_module) == "physboot");
    require(std::string_view(boot_options.kernel_name) == "vmours");
    require(ours::phys::apply_boot_options(
                named_options,
                options_from_literal("kernel.phys.next=netboot\nkernel.name=vmours.debug\n")) ==
            ours::Status::Ok);
    require(std::string_view(named_options.next_module) == "netboot");
    require(std::string_view(named_options.kernel_name) == "vmours.debug");

    // A name that does not fit the BootOptions buffer is malformed input.
    auto const too_long = std::string("kernel.phys.next=") + std::string(64, 'x');
    auto long_options = ours::phys::BootOptions{};
    require(ours::phys::apply_boot_options(long_options, options_from_literal(too_long)) ==
            ours::Status::InvalidArgument);
    return 0;
}
