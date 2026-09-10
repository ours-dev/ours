/// obit modify — add or remove items from an existing OBI container.
#include <deque>
#include <print>

#include "command.hpp"
#include "common.hpp"
#include <cxxopts.hpp>
#include <obi/package_builder.hpp>

namespace obit {
namespace {

    auto modify_add(std::string const &input_path, std::string const &output_path,
                    std::vector<std::string> const &specs) -> int {
        // ItemSource holds a non-owning Span, so file bytes read for new
        // items must outlive builder.build(); a deque never relocates
        // existing elements, so pointers into it stay valid throughout.
        std::deque<std::vector<u8>> arena;
        auto const data = read_file(input_path);
        if (!data) {
            std::println(std::cerr, "obit modify: cannot read: {}", input_path);
            return 2;
        }
        auto container = ContainerView::from(data->data(), data->size());
        if (container.is_err()) {
            std::println(std::cerr, "obit modify: invalid container: {}", input_path);
            return 1;
        }

        obi::PackageBuilder builder;
        // Copy existing items.
        for (usize i = 0; i < container.unwrap().item_count(); ++i) {
            auto item = container.unwrap().item(i);
            if (item.is_err()) {
                std::println(std::cerr, "obit modify: cannot read item {}", i);
                return 1;
            }
            auto const &v = item.unwrap();
            obi::ItemSource src;
            src.name = std::string(v.name());
            src.type = v.type();
            src.format = v.format();
            src.data = v.payload();
            src.flags = v.flags();
            src.memory_size = v.memory_size();
            src.alignment = v.alignment();
            src.compression = v.compression();
            if (auto result = builder.add_item(src); result.is_err()) {
                std::println(std::cerr, "obit modify: cannot add item '{}': status {}", src.name,
                             static_cast<int>(result.unwrap_err()));
                return 1;
            }
        }

        // Add new items from specs: NAME:TYPE:FORMAT:FILE
        for (auto const &spec: specs) {
            auto const c1 = spec.find(':');
            auto const c2 = spec.find(':', c1 + 1);
            auto const c3 = spec.find(':', c2 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos) {
                std::println(std::cerr, "obit modify: invalid spec: {}", spec);
                return 2;
            }
            auto const name = spec.substr(0, c1);

            auto const type_str = spec.substr(c1 + 1, c2 - c1 - 1);
            auto type = obi::ItemType::Data;
            if (type_str == "executable") {
                type = obi::ItemType::Executable;
            } else if (type_str == "config") {
                type = obi::ItemType::Config;
            } else if (type_str == "data") {
                type = obi::ItemType::Data;
            } else if (type_str == "kernel_storage") {
                type = obi::ItemType::StorageKernel;
            }

            auto const fmt_str = spec.substr(c2 + 1, c3 - c2 - 1);
            auto format = obi::ItemFormat::Raw;
            if (fmt_str == "elf") {
                format = obi::ItemFormat::Elf;
            } else if (fmt_str == "raw") {
                format = obi::ItemFormat::Raw;
            } else if (fmt_str == "yaml") {
                format = obi::ItemFormat::Yaml;
            }

            auto filepath = spec.substr(c3 + 1);
            auto file_data = read_file(filepath);
            if (!file_data) {
                std::println(std::cerr, "obit modify: cannot read: {}", filepath);
                return 2;
            }
            arena.push_back(std::move(*file_data));
            auto const &owned = arena.back();

            obi::ItemSource src;
            src.name = name;
            src.type = type;
            src.format = format;
            src.data = {owned.data(), owned.size()};
            src.flags = obi::ItemFlags::Required;
            src.memory_size = owned.size();
            if (auto result = builder.add_item(src); result.is_err()) {
                std::println(std::cerr, "obit modify: cannot add item '{}': status {}", src.name,
                             static_cast<int>(result.unwrap_err()));
                return 1;
            }
        }

        auto const new_container = builder.build(/*crc=*/false);
        if (new_container.empty()) {
            std::println(std::cerr, "obit modify: no items");
            return 1;
        }
        if (!write_file(output_path, {new_container.data(), new_container.size()})) {
            std::println(std::cerr, "obit modify: cannot write: {}", output_path);
            return 2;
        }
        return 0;
    }

    auto modify_remove(std::string const &input_path, std::string const &name,
                       std::string const &output_path) -> int {
        auto const data = read_file(input_path);
        if (!data) {
            std::println(std::cerr, "obit modify: cannot read: {}", input_path);
            return 2;
        }
        auto container = ContainerView::from(data->data(), data->size());
        if (container.is_err()) {
            std::println(std::cerr, "obit modify: invalid container: {}", input_path);
            return 1;
        }

        bool found = false;
        obi::PackageBuilder builder;
        for (usize i = 0; i < container.unwrap().item_count(); ++i) {
            auto item = container.unwrap().item(i);
            if (item.is_err()) {
                return 1;
            }
            auto const &v = item.unwrap();
            if (v.name() == name) {
                found = true;
                continue; // skip this item
            }
            obi::ItemSource src;
            src.name = std::string(v.name());
            src.type = v.type();
            src.format = v.format();
            src.data = v.payload();
            src.flags = v.flags();
            src.memory_size = v.memory_size();
            src.alignment = v.alignment();
            src.compression = v.compression();
            if (auto result = builder.add_item(src); result.is_err()) {
                std::println(std::cerr, "obit modify: cannot add item '{}': status {}", src.name,
                             static_cast<int>(result.unwrap_err()));
                return 1;
            }
        }

        if (!found) {
            std::println(std::cerr, "obit modify: item not found: {}", name);
            return 1;
        }
        auto const new_container = builder.build(/*crc=*/false);
        if (!write_file(output_path, {new_container.data(), new_container.size()})) {
            std::println(std::cerr, "obit modify: cannot write: {}", output_path);
            return 2;
        }
        return 0;
    }

    class ModifyCommand final: public AutoCommand<ModifyCommand> {
      public:
        ModifyCommand()
            : AutoCommand("modify", "add or remove items in an OBI container",
                          "add|remove INPUT [NAME|SPEC...] OUTPUT") {}

        auto execute(std::vector<std::string> const &args) const -> int override {
            // "add"/"remove" is a nested subcommand selector — cxxopts has no
            // concept of subcommands, so this one token stays a manual switch
            // (same reasoning as the top-level command dispatch in main.cpp);
            // everything after it is a normal flat cxxopts parse.
            if (args.empty()) {
                std::println(std::cerr, "obit modify: usage: obit modify add INPUT OUTPUT SPEC...\n"
                                        "                obit modify remove INPUT NAME OUTPUT");
                return 2;
            }
            auto const &sub = args[0];
            std::vector<std::string> const rest(args.begin() + 1, args.end());

            if (sub == "add") {
                cxxopts::Options opts("obit modify add", "add items to an OBI container");
                auto adder = opts.add_options();
                adder("input", "input OBI path", cxxopts::value<std::string>());
                adder("output", "output OBI path", cxxopts::value<std::string>());
                adder("specs", "NAME:TYPE:FORMAT:FILE ...",
                      cxxopts::value<std::vector<std::string>>());
                opts.parse_positional({"input", "output", "specs"});
                opts.positional_help("INPUT OUTPUT SPEC...");

                auto const argv = make_argv("obit modify add", rest);
                cxxopts::ParseResult parsed;
                try {
                    parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
                } catch (std::exception const &ex) {
                    std::println(std::cerr, "obit modify add: {}", ex.what());
                    return 2;
                }
                if (parsed.count("input") == 0 || parsed.count("output") == 0 ||
                    parsed.count("specs") == 0) {
                    std::println(std::cerr, "obit modify add: expected INPUT OUTPUT SPEC...");
                    return 2;
                }
                return modify_add(parsed["input"].as<std::string>(),
                                  parsed["output"].as<std::string>(),
                                  parsed["specs"].as<std::vector<std::string>>());
            }
            if (sub == "remove") {
                cxxopts::Options opts("obit modify remove", "remove an item from an OBI container");
                auto adder = opts.add_options();
                adder("input", "input OBI path", cxxopts::value<std::string>());
                adder("name", "item name to remove", cxxopts::value<std::string>());
                adder("output", "output OBI path", cxxopts::value<std::string>());
                opts.parse_positional({"input", "name", "output"});
                opts.positional_help("INPUT NAME OUTPUT");

                auto const argv = make_argv("obit modify remove", rest);
                cxxopts::ParseResult parsed;
                try {
                    parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
                } catch (std::exception const &ex) {
                    std::println(std::cerr, "obit modify remove: {}", ex.what());
                    return 2;
                }
                if (parsed.count("input") == 0 || parsed.count("name") == 0 ||
                    parsed.count("output") == 0) {
                    std::println(std::cerr, "obit modify remove: expected INPUT NAME OUTPUT");
                    return 2;
                }
                return modify_remove(parsed["input"].as<std::string>(),
                                     parsed["name"].as<std::string>(),
                                     parsed["output"].as<std::string>());
            }
            std::println(std::cerr, "obit modify: unknown subcommand: {}", sub);
            return 2;
        }
    };

} // namespace
} // namespace obit
