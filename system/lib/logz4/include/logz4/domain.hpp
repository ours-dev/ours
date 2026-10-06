/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_DOMAIN_HPP
#define LOGZ4_DOMAIN_HPP 1

#include <logz4/filter.hpp>
#include <logz4/level.hpp>

#include <atomic>
#include <optional>
#include <span>

#include <ours/compiler.hpp>
#include <ours/types.hpp>
#include <ustl/views/string_view.hpp>

/// The linker section every Domain is placed in, which the image's linker
/// script bounds into the one array Domain::all() walks -- the way
/// GKTL_COMMAND records land in .gktl.commands.
///
/// A macro only because the spelling depends on the object format, which is
/// build configuration: ELF takes a bare name, and this one is a C identifier
/// so that a default GNU ld/LLD layout (a hosted test) also defines
/// __start_logz4_domains/__stop_logz4_domains for it; Mach-O, where the host
/// unit test is linked on macOS, wants "segment,section" and bounds it with
/// section$start$/section$end$ symbols instead (src/domain.cpp).
#if defined(__MACH__)
#define LOGZ4_DOMAIN_SECTION LINK_SECTION("__DATA,__logz4_domains")
#else
#define LOGZ4_DOMAIN_SECTION LINK_SECTION("logz4_domains")
#endif

namespace logz4 {

/// The spelling of a domain's name, carried as a template argument.
///
/// It is a template argument, not a constructor argument, so that the name
/// itself is what identifies the Domain: every mention of `"kshell"` in an
/// image instantiates the one `Domain::named<"kshell">`, however many
/// translation units spell it. A per-domain level set from the console
/// therefore reaches every line of that domain, not just the lines of the
/// file whose copy happened to be found first.
///
/// The members are public because a class type used as a template argument
/// must be structural; nothing outside this header is expected to touch them.
template <usize N>
struct DomainName final {
    // Implicit on purpose: `Domain::named<"kshell">` converts the literal
    // through this constructor, and an explicit one would reject it.
    consteval DomainName(char const (&text)[N]) { // NOLINT(google-explicit-constructor)
        static_assert(N > 1, "a log domain needs a non-empty name");
        for (usize i = 0; i + 1 < N; ++i) {
            // The console splits its input on blanks, so a name containing
            // one could be printed but never typed back at `log level`.
            if (text[i] <= ' ' || text[i] > '~') {
                domain_name_must_be_printable_without_blanks();
            }
            chars[i] = text[i];
        }
    }

    /// Never defined. Reaching it during constant evaluation makes the
    /// template argument ill-formed, and the compiler's note names this
    /// function -- the nearest thing to a readable static_assert that a
    /// loop over a constructor argument can produce.
    static auto domain_name_must_be_printable_without_blanks() -> void;

    char chars[N]{};
};

/// A named log domain: who a line came from, and the level floor that
/// applies to it.
///
/// A Domain does two jobs: its name is printed on every line logged in it
/// (`[warn] (kshell) ...`) and recorded in the ring, and it carries its own
/// level floor so one subsystem's debug output can be turned on without
/// turning on everyone's.
///
/// Code does not log *through* a Domain; it enters one, with LogDomain below,
/// and logs with the ordinary free functions. A Domain is what the console
/// finds by name (`log level kshell debug`) and what the ring filters on.
///
/// A Domain cannot be constructed any other way than `NAMED`, and that is
/// what lets the name stand for the object (see DomainName above) -- and,
/// since `NAMED` places every instance in LOGZ4_DOMAIN_SECTION, what makes
/// all() the complete list.
class Domain final {
  public:
    template <DomainName Name>
    static Domain NAMED;

    Domain(Domain const &) = delete;
    auto operator=(Domain const &) -> Domain & = delete;

    /// Every Domain linked into this image, in link order.
    ///
    /// Read off the linker section rather than collected as each domain
    /// first logs, so a domain is listed from the moment it is linked in:
    /// `log level kshell debug` has to work before kshell has said anything,
    /// or there would be no way to see its first debug line.
    ///
    /// The list is per image. A phys module's domains sit in the module's own
    /// section, not in physload's, and listing them would mean registering
    /// each module's section as it is loaded. Nothing in phys needs them yet;
    /// it has no console.
    [[nodiscard]]
    static auto all() -> std::span<Domain>;

    /// The domain called `name`, or null. A linear scan: an image holds tens
    /// of domains, and this runs when someone types a console command.
    [[nodiscard]]
    static auto find(ustl::views::StringView name) -> Domain *;

    [[nodiscard]]
    auto name() const -> char const * {
        return m_name;
    }

    /// The floor this domain filters at: its own when one has been set,
    /// otherwise the global floor. Following the global floor is the
    /// default, so `log level warn` still quietens every domain nobody has
    /// singled out -- the same precedence as `RUST_LOG=warn,kshell=debug`.
    [[nodiscard]]
    auto min_level() const -> Level {
        if (auto const own = own_min_level(); own.has_value()) {
            return *own;
        }
        return logz4::min_level();
    }

    /// This domain's own floor, or nullopt while it follows the global one.
    [[nodiscard]]
    auto own_min_level() const -> std::optional<Level> {
        auto const level = m_min_level.load(std::memory_order_relaxed);
        if (level == FOLLOW_GLOBAL) {
            return std::nullopt;
        }
        return level;
    }

    /// Give this domain a floor of its own. It may be looser than the
    /// global one as well as stricter: that is the point of having it.
    auto set_min_level(Level level) -> void {
        m_min_level.store(level, std::memory_order_relaxed);
    }

    /// Drop this domain's own floor; it follows the global one again.
    auto clear_min_level() -> void {
        m_min_level.store(FOLLOW_GLOBAL, std::memory_order_relaxed);
    }

    [[nodiscard]]
    auto is_enabled(Level level) const -> bool {
        return level >= min_level();
    }

  private:
    /// Not a Level: a value outside the enum's range, stored in the same
    /// atomic so that reading "own floor or none" stays one relaxed load.
    /// A separate flag would need two loads and could be seen half-updated.
    static constexpr auto FOLLOW_GLOBAL = static_cast<Level>(0xFF);

    constexpr explicit Domain(char const *name) : m_name(name) {}

    /// Points into the DomainName template argument object, which has static
    /// storage duration, so the name outlives every line that prints it.
    char const *m_name;

    /// Relaxed, like the global floor: the gate is advisory, and a line
    /// racing a `log level` command may land on either side of it.
    std::atomic<Level> m_min_level{FOLLOW_GLOBAL};
};

// constinit: a domain must be usable by the very first line an image logs,
// before any dynamic initializer has run -- constant initialization is what
// guarantees that, and it is also why the level lives in the object instead
// of a table filled in at start-up.
//
// Each instantiation is a COMDAT member in LOGZ4_DOMAIN_SECTION, so the
// linker keeps one copy per name and all() sees each name once.
template <DomainName Name>
LOGZ4_DOMAIN_SECTION
constinit Domain Domain::NAMED{Name.chars};

/// Where "the current domain" of the running context is kept: the slot the
/// free functions read and LogDomain writes.
///
/// It has to follow whatever the image calls a context, which logz4 cannot
/// know, so the image plugs it in -- the way set_clock() and set_cpu() plug
/// in time and CPU. `slot` returns a reference to the running context's own
/// slot. Until something is installed, and in images that never install
/// anything (phys, the host test), there is one slot for the whole image,
/// which is right for a single thread of execution and nothing else.
///
/// A kernel must install a per-thread slot, not a per-CPU one: a thread may
/// sleep inside a LogDomain scope, and whatever runs on its CPU meanwhile
/// would otherwise log under a domain it never entered.
auto set_domain_slot(Domain const *&(*slot)()) -> void;

/// The running context's slot, through whatever set_domain_slot() installed.
///
/// Exported rather than inline: a phys module's LogDomain has to write the
/// slot physload's vlog() reads, and an inline accessor would be the
/// module's own hidden copy.
PHYS_EXPORT
auto current_domain_slot() -> Domain const *&;

/// The domain the running context is in, or null outside every LogDomain.
[[nodiscard]]
inline auto current_domain() -> Domain const * {
    return current_domain_slot();
}

/// Puts everything logged in its scope into the domain `Name`:
///
///     {
///         logz4::LogDomain<"kshell"> domain;
///         log::trace("Heelo");       // [trace] (kshell) Heelo
///     }
///     log::trace("Workd");           // [trace] Workd
///
/// The scope is dynamic, not lexical: a function called from inside it logs
/// in the domain too, wherever that function is written. The domain is
/// context that travels with the thread of execution, not a name fixed per
/// file at compile time. A scope that should not claim its callees' lines
/// has to end before it calls them.
///
/// Scopes nest: an inner LogDomain wins for its extent and hands back the
/// outer one when it ends. The guard remembers which slot it wrote and the
/// value it replaced, so leaving restores exactly that -- even if the
/// context's slot changed meanwhile (a kernel installs its per-thread hook
/// after boot has already started logging).
///
/// Leaving is the destructor, so a guard must be a named local. A bare
/// `LogDomain<"kshell">{};` enters and leaves on the same line; the
/// constructor is [[nodiscard]] to make the compiler say so.
template <DomainName Name>
class LogDomain final {
  public:
    [[nodiscard("a LogDomain only lasts as long as the named object holding it")]]
    LogDomain()
        : m_slot(&current_domain_slot()), m_outer(*m_slot) {
        *m_slot = &Domain::NAMED<Name>;
    }

    ~LogDomain() {
        *m_slot = m_outer;
    }

    LogDomain(LogDomain const &) = delete;
    auto operator=(LogDomain const &) -> LogDomain & = delete;

  private:
    /// Not re-fetched on the way out: the running context's slot is only
    /// the right one to restore if it is the one written on the way in.
    Domain const **m_slot;
    Domain const *m_outer;
};

} // namespace logz4

#endif // LOGZ4_DOMAIN_HPP
