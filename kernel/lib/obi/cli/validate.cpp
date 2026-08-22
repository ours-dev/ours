#include "command.hpp"
#include "common.hpp"

#include <print>

#include <cxxopts.hpp>

namespace obit {

class ValidateCommand final: public AutoCommand<ValidateCommand> {
  public:
    ValidateCommand() : AutoCommand("validate", "validate an OBI container", "INPUT") {}

    auto execute(std::vector<std::string> const &args) const -> int override {
        cxxopts::Options opts("obit validate", "validate an OBI container");
        opts.add_options()("input", "OBI container path", cxxopts::value<std::string>());
        opts.parse_positional({"input"});
        opts.positional_help("INPUT");

        auto const argv = make_argv("obit validate", args);
        cxxopts::ParseResult parsed;
        try {
            parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
        } catch (std::exception const &ex) {
            std::println(std::cerr, "obit validate: {}", ex.what());
            return 2;
        }
        if (parsed.count("input") == 0) {
            std::println(std::cerr, "obit validate: expected INPUT");
            return 2;
        }

        auto const input = parsed["input"].as<std::string>();
        auto const container = open_container(input);
        if (!container) {
            std::println(std::cerr, "obit: {}: {}", message(container.error()), input);
            return (container.error() == Error::Io) ? 2 : 1;
        }
        return 0;
    }
};

} // namespace obit
