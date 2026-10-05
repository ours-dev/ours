/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_AUTORUN_HPP
#define INIT_AUTORUN_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

namespace init {
/// The option naming a program to start once the shell is up, as
/// `ours.autorun.boot=<name>[+arg...]`: a BootFS file name and its
/// arguments, `+` standing for the spaces a kernel command line cannot hold
/// inside one word. The first word is a BootFS name, not a path: the program
/// is looked up in the BootFS, not in a namespace.
inline constexpr std::string_view AUTORUN_OPTION{"ours.autorun.boot"};

/// An `ours.autorun.boot` value split into the argv it runs: the first word
/// is the BootFS name, and all of them, the name included, are the argv.
///
/// Its own header, apart from init.cpp, so that the host can test it: init
/// itself exists only in the user toolchain, and the splitting is the one
/// part of autorun that needs no kernel to check.
class AutorunCommand {
  public:
    /// How many words -- the name and its arguments -- a command may have.
    /// More is refused rather than cut short: a truncated argv runs a command
    /// nobody wrote.
    static constexpr u32 MAX_WORDS = 8;

    AutorunCommand() = default;

    /// Splits |command| on `+` and on Ok makes |out| hold the words, which
    /// point into |command| and so must not outlive it.
    ///
    /// Empty words are skipped, so `a++b` and a trailing `+` mean what they
    /// look like. The words become the argv directly, with no shell in
    /// between to read them a second time.
    ///
    ///   InvArgs     no words at all: nothing names a program;
    ///   OutOfRange  more than MAX_WORDS.
    ///
    /// |out| is untouched unless the answer is Ok.
    [[nodiscard]]
    static constexpr auto parse(std::string_view command, AutorunCommand *const out)
        -> ours::Status {
        AutorunCommand parsed;
        while (!command.empty()) {
            auto const end = command.find('+');
            auto const word = command.substr(0, end);
            if (!word.empty()) {
                if (parsed.m_count == MAX_WORDS) {
                    return ours::Status::OutOfRange;
                }
                parsed.m_words[parsed.m_count++] = word;
            }
            if (end == std::string_view::npos) {
                break;
            }
            command.remove_prefix(end + 1);
        }
        if (parsed.m_count == 0) {
            return ours::Status::InvArgs;
        }
        *out = parsed;
        return ours::Status::Ok;
    }

    /// The BootFS name of the program to start; argv[0] as well.
    [[nodiscard]]
    constexpr auto name() const -> std::string_view {
        return m_words[0];
    }

    /// The whole argv, name first.
    [[nodiscard]]
    constexpr auto words() const -> std::span<std::string_view const> {
        return {m_words, m_count};
    }

  private:
    std::string_view m_words[MAX_WORDS];
    u32 m_count = 0;
};
} // namespace init

#endif // #ifndef INIT_AUTORUN_HPP
