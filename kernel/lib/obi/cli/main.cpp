#include <format>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include <cstdlib>

#include "command.hpp"

namespace obit {

// Dispatch only: argv[1] selects the command, everything after is passed
// through unparsed — each Command parses its own arguments (see pack.cpp,
// modify.cpp, etc.).  A generic top-level option parser would have to
// know every subcommand's flags in advance to avoid rejecting them as
// unrecognized, so commands own their own argument grammar instead.
class Tool final {
  public:
    auto run(int argc, char **argv) -> int;

  private:
    auto usage() const -> std::string;
};

auto Tool::usage() const -> std::string {
    auto out = std::format("obit — OBI container tool\n\n"
                           "Usage:\n"
                           "  obit COMMAND [ARGS...]\n\n"
                           "Commands:\n");
    auto const &all = CommandRegistry::instance().all();
    for (auto const &[name, cmd]: all) {
        out += std::format("  {:<12} {}  {}\n", name, cmd->usage_args(), cmd->description());
    }
    return out;
}

auto Tool::run(int const argc, char **const argv) -> int {
    if (argc < 2) {
        std::println("{}", usage());
        return 0;
    }

    std::string const first = argv[1];
    if (first == "--help" || first == "-h") {
        std::println("{}", usage());
        return 0;
    }

    auto const *const cmd = CommandRegistry::instance().find(first);
    if (!cmd) {
        std::println(std::cerr, "obit: unknown command: {}\n\n{}", first, usage());
        return 2;
    }

    std::vector<std::string> args(argv + 2, argv + argc);
    return cmd->execute(args);
}
} // namespace obit

auto main(int argc, char **argv) -> int {
    obit::Tool tool;
    return tool.run(argc, argv);
}
