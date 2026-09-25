/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_TARGET_HPP
#define LOGZ4_TARGET_HPP 1

#include <logz4/filter.hpp>
#include <logz4/level.hpp>
#include <logz4/log.hpp>

#include <atomic>
#include <optional>
#include <span>

#include <ours/compiler.hpp>
#include <ours/types.hpp>
#include <ustl/util/move.hpp>
#include <ustl/views/string_view.hpp>

/// The linker section every Target is placed in, which the image's linker
/// script bounds into the one array Target::all() walks -- the way
/// GKTL_COMMAND records land in .gktl.commands, and Linux's dynamic_debug
/// descriptors in __dyndbg.
///
/// A macro only because the spelling depends on the object format, which is
/// build configuration: ELF takes a bare name, and this one is a C identifier
/// so that a default GNU ld/LLD layout (a hosted test) also defines
/// __start_logz4_targets/__stop_logz4_targets for it; Mach-O, where the host
/// unit test is linked on macOS, wants "segment,section" and bounds it with
/// section$start$/section$end$ symbols instead (src/target.cpp).
#if defined(__MACH__)
#define LOGZ4_TARGET_SECTION LINK_SECTION("__DATA,__logz4_targets")
#else
#define LOGZ4_TARGET_SECTION LINK_SECTION("logz4_targets")
#endif

namespace logz4 {

/// The spelling of a target's name, carried as a template argument.
///
/// It is a template argument, not a constructor argument, so that the name
/// itself is what identifies the Target: every mention of `"kshell"` in an
/// image instantiates the one `Target::named<"kshell">`, however many
/// translation units spell it. A per-target level set from the console
/// therefore reaches every line of that target, not just the lines of the
/// file whose copy happened to be found first.
///
/// The members are public because a class type used as a template argument
/// must be structural; nothing outside this header is expected to touch them.
template <usize N>
struct TargetName final {
    // Implicit on purpose: `Target::named<"kshell">` converts the literal
    // through this constructor, and an explicit one would reject it.
    consteval TargetName(char const (&text)[N]) { // NOLINT(google-explicit-constructor)
        static_assert(N > 1, "a log target needs a non-empty name");
        for (usize i = 0; i + 1 < N; ++i) {
            // The console splits its input on blanks, so a name containing
            // one could be printed but never typed back at `log level`.
            if (text[i] <= ' ' || text[i] > '~') {
                target_name_must_be_printable_without_blanks();
            }
            chars[i] = text[i];
        }
    }

    /// Never defined. Reaching it during constant evaluation makes the
    /// template argument ill-formed, and the compiler's note names this
    /// function -- the nearest thing to a readable static_assert that a
    /// loop over a constructor argument can produce.
    static auto target_name_must_be_printable_without_blanks() -> void;

    char chars[N]{};
};

/// A named log target: who a line came from.
///
/// The Rust `log` crate calls this a target (filtered with
/// `RUST_LOG=kshell=debug`), GLib a domain (`G_LOG_DOMAIN`), Android and
/// Fuchsia syslog a tag. Linux has no single object for it; a file defines
/// `pr_fmt` to prefix its lines and dynamic_debug switches pr_debug() sites
/// per module. A Target does both jobs: its name is printed on every line
/// (`[warn] (kshell) ...`) and recorded in the ring, and it carries its own
/// level floor so one subsystem's debug output can be turned on without
/// turning on everyone's.
///
///     constexpr auto &KSHELL = logz4::Target::named<"kshell">;
///     KSHELL.warn("failed to spawn console thread");
///
/// A Target cannot be constructed any other way. `named` is the only
/// constructor call in the tree, and that is what lets the name stand for
/// the object (see TargetName above) -- and, since `named` places every
/// instance in LOGZ4_TARGET_SECTION, what makes all() the complete list.
///
/// The free functions (`log::warn(...)`) remain: they are the anonymous
/// target, filtered by the global floor alone and printed without a name,
/// so existing call sites keep compiling and can move over one at a time.
class Target final {
  public:
    template <TargetName Name>
    static Target named;

    Target(Target const &) = delete;
    auto operator=(Target const &) -> Target & = delete;

    /// Every Target linked into this image, in link order.
    ///
    /// Read off the linker section rather than collected as each target
    /// first logs, so a target is listed from the moment it is linked in:
    /// `log level kshell debug` has to work before kshell has said anything,
    /// or there would be no way to see its first debug line.
    ///
    /// The list is per image. A phys module's targets sit in the module's own
    /// section, not in physload's -- the same split as Linux, whose modules
    /// each carry a __dyndbg section that ddebug_add_module() registers at
    /// load time. Nothing in phys needs them yet; it has no console.
    [[nodiscard]]
    static auto all() -> std::span<Target>;

    /// The target called `name`, or null. A linear scan: an image holds tens
    /// of targets, and this runs when someone types a console command.
    [[nodiscard]]
    static auto find(ustl::views::StringView name) -> Target *;

    [[nodiscard]]
    auto name() const -> char const * {
        return m_name;
    }

    /// The floor this target filters at: its own when one has been set,
    /// otherwise the global floor. Following the global floor is the
    /// default, so `log level warn` still quietens every target nobody has
    /// singled out -- the same precedence as `RUST_LOG=warn,kshell=debug`.
    [[nodiscard]]
    auto min_level() const -> Level {
        if (auto const own = own_min_level(); own.has_value()) {
            return *own;
        }
        return logz4::min_level();
    }

    /// This target's own floor, or nullopt while it follows the global one.
    [[nodiscard]]
    auto own_min_level() const -> std::optional<Level> {
        auto const level = m_min_level.load(std::memory_order_relaxed);
        if (level == FOLLOW_GLOBAL) {
            return std::nullopt;
        }
        return level;
    }

    /// Give this target a floor of its own. It may be looser than the
    /// global one as well as stricter: that is the point of having it.
    auto set_min_level(Level level) -> void {
        m_min_level.store(level, std::memory_order_relaxed);
    }

    /// Drop this target's own floor; it follows the global one again.
    auto clear_min_level() -> void {
        m_min_level.store(FOLLOW_GLOBAL, std::memory_order_relaxed);
    }

    [[nodiscard]]
    auto is_enabled(Level level) const -> bool {
        return level >= min_level();
    }

    template <typename... Args>
    auto log(Level level, ustl::views::StringView fmt, Args &&...args) const -> void {
        detail::emit(this, level, fmt, ustl::forward<Args>(args)...);
    }

    // The same compile-time gate as the free debug()/trace(): in an image
    // built without KERNEL_DEBUG_LEVEL these calls vanish, whatever floor the
    // target is later given -- a per-target runtime floor can only let
    // through what the build kept. The call sits *inside* the `if constexpr`
    // so that it is a discarded statement: not even instantiated, rather
    // than instantiated and left for the optimizer to delete.
    template <typename... Args>
    auto trace(ustl::views::StringView fmt, Args &&...args) const -> void {
        if constexpr (LOGZ4_VERBOSE_LOGGING) {
            log(Level::Trace, fmt, ustl::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    auto debug(ustl::views::StringView fmt, Args &&...args) const -> void {
        if constexpr (LOGZ4_VERBOSE_LOGGING) {
            log(Level::Debug, fmt, ustl::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    auto info(ustl::views::StringView fmt, Args &&...args) const -> void {
        log(Level::Info, fmt, ustl::forward<Args>(args)...);
    }

    template <typename... Args>
    auto warn(ustl::views::StringView fmt, Args &&...args) const -> void {
        log(Level::Warn, fmt, ustl::forward<Args>(args)...);
    }

    template <typename... Args>
    auto error(ustl::views::StringView fmt, Args &&...args) const -> void {
        log(Level::Error, fmt, ustl::forward<Args>(args)...);
    }

  private:
    /// Not a Level: a value outside the enum's range, stored in the same
    /// atomic so that reading "own floor or none" stays one relaxed load.
    /// A separate flag would need two loads and could be seen half-updated.
    static constexpr auto FOLLOW_GLOBAL = static_cast<Level>(0xFF);

    constexpr explicit Target(char const *name) : m_name(name) {}

    /// Points into the TargetName template argument object, which has static
    /// storage duration, so the name outlives every line that prints it.
    char const *m_name;

    /// Relaxed, like the global floor: the gate is advisory, and a line
    /// racing a `log level` command may land on either side of it.
    std::atomic<Level> m_min_level{FOLLOW_GLOBAL};
};

// constinit: a target must be usable by the very first line an image logs,
// before any dynamic initializer has run -- constant initialization is what
// guarantees that, and it is also why the level lives in the object instead
// of a table filled in at start-up.
//
// Each instantiation is a COMDAT member in LOGZ4_TARGET_SECTION, so the
// linker keeps one copy per name and all() sees each name once.
template <TargetName Name>
LOGZ4_TARGET_SECTION constinit Target Target::named{Name.chars};

} // namespace logz4

#endif // LOGZ4_TARGET_HPP
