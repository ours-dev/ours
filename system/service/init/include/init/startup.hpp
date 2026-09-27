/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_STARTUP_HPP
#define INIT_STARTUP_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <string_view>

#include <init/autorun.hpp>

namespace init {
/// Where init's startup list lives in the BootFS it is given. The build writes
/// it there -- kernel_package(init_startup = true) gathers every
/// init_startup() entry an image lists (//system/service/init/init_startup.gni)
/// -- and marks it user-visible, which is what lets init see it at all.
///
/// Fuchsia's component_manager reads its root manifest from a path in the
/// boot image the same way (root.cml, and bootstrap.cml's `children` with
/// `startup: "eager"`); this file is ours' counterpart of that list of eager
/// children, flattened to one line per child.
inline constexpr std::string_view STARTUP_PATH{"config/init/startup"};

/// One startup handle an entry may ask for. Each word the build accepts in
/// init_startup_capabilities (init_startup.gni) is one of these; the two
/// lists are a contract and must grow together.
enum class Capability : u8 {
    /// `init`: User0, a channel whose other end init keeps and runs its
    /// liveness check on -- the one handle a program needs to be supervised.
    Init = 1U << 0,
    /// `bootfs`: VmoBootfs, a duplicate of init's BootFS VMO.
    Bootfs = 1U << 1,
    /// `job`: JobDefault, a job to start programs under.
    Job = 1U << 2,
};

/// The set of capabilities one entry asks for.
///
/// A type rather than a bare mask so that the only way to fill one is by
/// naming a Capability: a bit no word stands for cannot get in, and init
/// asks "is this one there" instead of testing bits by hand.
class Capabilities {
  public:
    constexpr Capabilities() = default;

    constexpr auto add(Capability const capability) -> void {
        m_bits |= static_cast<u8>(capability);
    }

    [[nodiscard]]
    constexpr auto contains(Capability const capability) const -> bool {
        return (m_bits & static_cast<u8>(capability)) != 0;
    }

    [[nodiscard]]
    constexpr auto empty() const -> bool {
        return m_bits == 0;
    }

  private:
    u8 m_bits = 0;
};

/// The capability |word| names, or NotSupported for a word init does not
/// know. Unknown is refused rather than ignored: an entry that asked for a
/// handle and is started without it would fail somewhere far from the cause.
[[nodiscard]]
constexpr auto parse_capability(std::string_view const word, Capability *const out)
    -> ours::Status {
    if (word == "init") {
        *out = Capability::Init;
    } else if (word == "bootfs") {
        *out = Capability::Bootfs;
    } else if (word == "job") {
        *out = Capability::Job;
    } else {
        return ours::Status::NotSupported;
    }
    return ours::Status::Ok;
}

/// One line of the startup list: `<command> [<capability>,...]`.
///
/// The command is the same `+`-separated shape as `ours.autorun.boot`, so it
/// is split by the same AutorunCommand::parse(): first word the BootFS name
/// and argv[0]. One space, then the capabilities, comma-separated; a line
/// with no space asks for none. A second space ends up inside a capability
/// word, which then names nothing and refuses the line -- the build never
/// writes one (init_startup.gni refuses separators in arguments).
///
/// Its own header, apart from init.cpp, for the reason AutorunCommand is:
/// init exists only in the user toolchain, and reading a line needs no kernel
/// to check.
class StartupEntry {
  public:
    StartupEntry() = default;

    /// Parses |line| and on Ok makes |out| hold it. The command's words point
    /// into |line| and so must not outlive it.
    ///
    /// Empty capability words are skipped, as empty `+` words are in the
    /// command, so a stray comma means what it looks like.
    ///
    ///   InvArgs       the command has no words;
    ///   OutOfRange    the command has more than AutorunCommand::MAX_WORDS;
    ///   NotSupported  a capability init does not know.
    ///
    /// |out| is untouched unless the answer is Ok.
    [[nodiscard]]
    static constexpr auto parse(std::string_view const line, StartupEntry *const out)
        -> ours::Status {
        auto const space = line.find(' ');
        StartupEntry parsed;
        if (auto const status = AutorunCommand::parse(line.substr(0, space), &parsed.m_command);
            status != ours::Status::Ok) {
            return status;
        }
        if (space != std::string_view::npos) {
            auto rest = line.substr(space + 1);
            while (!rest.empty()) {
                auto const end = rest.find(',');
                auto const word = rest.substr(0, end);
                if (!word.empty()) {
                    auto capability = Capability::Init;
                    if (auto const status = parse_capability(word, &capability);
                        status != ours::Status::Ok) {
                        return status;
                    }
                    parsed.m_capabilities.add(capability);
                }
                if (end == std::string_view::npos) {
                    break;
                }
                rest.remove_prefix(end + 1);
            }
        }
        *out = parsed;
        return ours::Status::Ok;
    }

    /// The program and its argv, as for autorun.
    [[nodiscard]]
    constexpr auto command() const -> AutorunCommand const & {
        return m_command;
    }

    [[nodiscard]]
    constexpr auto capabilities() const -> Capabilities {
        return m_capabilities;
    }

  private:
    AutorunCommand m_command;
    Capabilities m_capabilities;
};

/// The startup list's text, taken a line at a time.
///
/// Only the splitting: each line is handed back whole, for the caller to
/// parse and, when that fails, to quote in its report. Keeping the two apart
/// is what lets one bad line be skipped without losing the ones after it.
/// Empty lines are skipped, so the newline the build ends the file with does
/// not read as an entry naming nothing.
class StartupLines {
  public:
    constexpr explicit StartupLines(std::string_view const text) : m_rest(text) {}

    /// Puts the next non-empty line, without its newline, in |line|; false
    /// once there are none left.
    [[nodiscard]]
    constexpr auto next(std::string_view *const line) -> bool {
        while (!m_rest.empty()) {
            auto const end = m_rest.find('\n');
            auto const candidate = m_rest.substr(0, end);
            if (end == std::string_view::npos) {
                m_rest = {};
            } else {
                m_rest.remove_prefix(end + 1);
            }
            if (!candidate.empty()) {
                *line = candidate;
                return true;
            }
        }
        return false;
    }

  private:
    std::string_view m_rest;
};
} // namespace init

#endif // #ifndef INIT_STARTUP_HPP
