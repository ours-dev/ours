#include <print>

#include "command.hpp"
#include "common.hpp"
#include <cxxopts.hpp>

namespace obit {

class ExtractCommand final: public AutoCommand<ExtractCommand> {
  public:
    ExtractCommand()
        : AutoCommand("extract", "extract an item from an OBI container", "INPUT NAME OUTPUT") {}

    auto execute(std::vector<std::string> const &args) const -> int override {
        cxxopts::Options opts("obit extract", "extract an item from an OBI container");
        auto adder = opts.add_options();
        adder("input", "OBI container path", cxxopts::value<std::string>());
        adder("name", "item name", cxxopts::value<std::string>());
        adder("output", "output file path", cxxopts::value<std::string>());
        opts.parse_positional({"input", "name", "output"});
        opts.positional_help("INPUT NAME OUTPUT");

        auto const argv = make_argv("obit extract", args);
        cxxopts::ParseResult parsed;
        try {
            parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
        } catch (std::exception const &ex) {
            std::println(std::cerr, "obit extract: {}", ex.what());
            return 2;
        }
        if (parsed.count("input") == 0 || parsed.count("name") == 0 ||
            parsed.count("output") == 0) {
            std::println(std::cerr, "obit extract: expected INPUT NAME OUTPUT");
            return 2;
        }

        auto const input = parsed["input"].as<std::string>();
        auto const container = open_container(input);
        if (!container) {
            std::println(std::cerr, "obit: {}: {}", message(container.error()), input);
            return (container.error() == Error::Io) ? 2 : 1;
        }

        auto const name = parsed["name"].as<std::string>();
        auto const item = find_item(container->view(), name);
        if (!item) {
            if (item.error() == Error::NotFound) {
                std::println(std::cerr, "obit: item not found: {}", name);
            } else {
                std::println(std::cerr, "obit: ambiguous item: {}", name);
            }
            return 1;
        }

        auto const &view = *item;
        std::vector<u8> materialized(view.memory_size());
        auto const result =
            view.materialize(MutableBytes(materialized.data(), materialized.size()));
        if (result.is_err()) {
            std::println(std::cerr, "obit: cannot materialize item");
            return 1;
        }
        auto const output = parsed["output"].as<std::string>();
        auto const wrote = write_file(output, Bytes(materialized.data(), materialized.size()));
        if (!wrote) {
            std::println(std::cerr, "obit: cannot write: {}", output);
            return 2;
        }
        return 0;
    }
};

} // namespace obit
