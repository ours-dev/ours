#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <obi/command_line_view.hpp>
#include <obi/container_view.hpp>
#include <obi/package_builder.hpp>
#include <ours/phys/boot_options.hpp>

#include <array>
#include <string>
#include <string_view>

#include <cstdlib>

namespace {
using std::operator""sv;

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
            ours::Status::InvArgs);

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
            ours::Status::InvArgs);

    // from_text(): a command line that never came out of an OBI item, which
    // is how a boot-shim forwards the one its legacy protocol handed it.
    // A bootloader hands over a C string, so one trailing NUL is trimmed
    // rather than rejected.
    require(obi::CommandLineView::from_text({}).is_ok());
    require(obi::CommandLineView::from_text("kernel.name=vmours\0"sv)
                .unwrap()
                .value("kernel.name")
                .value() == "vmours");
    // An interior NUL is not a command line, it is two of them.
    require(obi::CommandLineView::from_text("kernel.name=a\0kernel.name=b"sv).is_err());
    require(obi::CommandLineView::from_text("\xC0\x80"sv).is_err());

    // physload layers the two sources: the container's Config item is what
    // the build baked in, the bootloader's command line is what the user
    // typed this boot, so the second one applied wins on any option both
    // spell while leaving the ones only the first names alone.
    auto layered = ours::phys::BootOptions{};
    require(ours::phys::apply_boot_options(
                layered, options_from_literal("kernel.name=vmours.debug\naslr.disabled=1\n")) ==
            ours::Status::Ok);
    require(ours::phys::apply_boot_options(
                layered, obi::CommandLineView::from_text("aslr.disabled=0"sv).unwrap()) ==
            ours::Status::Ok);
    require(std::string_view(layered.kernel_name) == "vmours.debug");
    require(!layered.aslr_disabled);

    auto log_options = ours::phys::BootOptions{};
    require(ours::phys::apply_boot_options(
                log_options, options_from_literal("log.level=error\n")) == ours::Status::Ok);
    require(logz4::min_level() == logz4::Level::Error);
    require(ours::phys::apply_boot_options(log_options, options_from_literal("log.level=nope\n")) ==
            ours::Status::InvArgs);
    return 0;
}
