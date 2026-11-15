/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_COMMAND_HPP
#define GKTL_COMMAND_HPP 1

#include <gktl/types.hpp>

#include <span>
#include <string_view>

namespace gktl {

/// The console line one command was invoked with: the command's own word and
/// the words after it, already split. The C shape the console hands over --
/// a count and pointers into the line buffer -- stops at run_command(); a
/// handler sees only this value, whose views alias the line for exactly as
/// long as the handler runs, the same lifetime the raw argv had.
class Invocation {
  public:
    Invocation(std::string_view const command, std::span<std::string_view const> const words)
        : m_command{command}, m_words{words} {}

    /// The command's own word: what was looked up, argv[0] in the console's
    /// shape.
    [[nodiscard]]
    auto command() const -> std::string_view {
        return m_command;
    }

    /// The words after the command, in the order the line carried them.
    [[nodiscard]]
    auto words() const -> std::span<std::string_view const> {
        return m_words;
    }

  private:
    std::string_view m_command;
    std::span<std::string_view const> m_words;
};

struct Command {
    using Callback = auto (*)(Invocation const &) -> int;

    char const *cmd;
    char const *help;
    Callback callback;
};

auto run_command(int argc, char **argv) -> void;

} // namespace gktl

#define GKTL_COMMAND(NAME, CMD, HANDLER, HELP)                                                     \
    namespace commands::NAME {                                                                     \
        LINK_SECTION(".gktl.commands")                                                             \
        FORCE_USED static ::gktl::Command const COMMAND_##NAME{                                    \
            .cmd = CMD, .help = HELP, .callback = HANDLER};                                        \
    }

#endif // #ifndef GKTL_COMMAND_HPP