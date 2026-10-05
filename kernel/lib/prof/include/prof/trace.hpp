/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_TRACE_HPP
#define PROF_TRACE_HPP 1

#include <prof/record_buffer.hpp>

#include <atomic>
#include <type_traits>
#include <utility>

#include <cstdint>

#include <ours/compiler.hpp>

/// Compile-time master switch. With it off every `emit` body below is empty,
/// so a tracepoint call and its `Scope` object disappear entirely -- not a
/// branch, not a byte. Note that this does *not* elide the arguments: a
/// function cannot skip evaluating what it was handed, which is the one
/// thing a macro would have bought. Keep tracepoint arguments cheap, or
/// guard an expensive one behind `Tracepoint::enabled()` explicitly.
#ifndef PROF_TRACE_ENABLED
#define PROF_TRACE_ENABLED 1
#endif

namespace prof {

inline constexpr bool TRACE_ENABLED = PROF_TRACE_ENABLED != 0;

/// What a scope measures. Chosen per session rather than per site: switching
/// a run from wall time to cache misses should not need a recompile, and the
/// same tracepoint is worth looking at through more than one lens.
///
/// `Cycles` and `Instructions` come from the fixed-function counters, which
/// no one else can take. The rest need a general-purpose PMU counter, which
/// whatever else owns the PMU may hold -- `Tracer` has to acquire one and
/// report failure rather than silently fighting over the same register.
enum class Metric : u8 {
    Timestamp,
    Cycles,
    Instructions,
    CacheMisses,
    BranchMisses,
};

/// Which end of a region, or neither.
///
/// A region is two records, not one carrying a duration. That costs twice
/// the records and makes the host pair them, and buys the case worth having:
/// a region that never finishes still leaves its `Begin` behind, so a hang
/// inside one is visible. A single end-of-region record would show nothing
/// at all there.
enum class EventKind : u8 {
    /// An unwritten slot. The sink does not know this layout, so this is
    /// what tells the exporter which records are real.
    Invalid = 0,
    Instant,
    Begin,
    End,
};

inline constexpr usize MAX_TRACE_ARGS = 4;

/// One event, as the host tool parses it. Fixed size, like the sampler's
/// record, and 56 bytes against that one's 152.
struct TraceRecord final {
    EventKind m_kind;
    u8 m_arg_count;
    u16 m_event_id;
    u16 m_cpu;
    u16 m_reserved;
    u32 m_tid;
    /// A timestamp, or the counter reading named by the session's `Metric`.
    /// The dump header says which, so the number is never ambiguous.
    u64 m_metric;
    u64 m_args[MAX_TRACE_ARGS];
};
static_assert(sizeof(TraceRecord) == 56);

/// Anything that fits a slot without the tracepoint having to say how.
/// Pointers go in as addresses; enums as their underlying value. Strings are
/// deliberately absent: a pointer to kernel memory means nothing to the host
/// tool, and interning is a table plus a registration policy that should not
/// be invented before a tracepoint actually needs one. Name the *event*
/// instead, or pass an enum.
template <typename T>
concept TraceArg = std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>;

/// How each accepted argument type reaches its slot. Pointers go in as
/// addresses and enums as their underlying value, so the host sees one
/// uniform 64-bit field and the tracepoint's declared signature is what says
/// how to read it back.
template <TraceArg T>
inline auto to_slot(T value) noexcept -> u64 {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<u64>(reinterpret_cast<std::uintptr_t>(value));
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<u64>(std::to_underlying(value));
    } else {
        return static_cast<u64>(value);
    }
}

/// A declared trace site.
///
/// Declared as a `constinit` object in the `.prof.tracepoints` link section,
/// the same way `GKTL_COMMAND` and `GKTL_INIT_HOOK` register theirs, so the
/// set is collected at link time with no static-initialisation order to
/// reason about. Ids are assigned from the section at boot and exported as a
/// name table ahead of the events, so the host decodes without hardcoding.
class Tracepoint final {
  public:
    consteval Tracepoint(char const *name, char const *category) noexcept
        : m_name(name), m_category(category) {}

    Tracepoint(Tracepoint const &) = delete;
    auto operator=(Tracepoint const &) -> Tracepoint & = delete;

    [[nodiscard]]
    auto name() const noexcept -> char const * {
        return m_name;
    }

    [[nodiscard]]
    auto category() const noexcept -> char const * {
        return m_category;
    }

    /// Whether this site is currently recording. Call it directly to guard
    /// an argument that is expensive to compute.
    [[nodiscard]]
    auto enabled() const noexcept -> bool {
        if constexpr (!TRACE_ENABLED) {
            return false;
        } else {
            return m_enabled.load(std::memory_order_relaxed);
        }
    }

    auto set_enabled(bool enabled) const noexcept -> void {
        m_enabled.store(enabled, std::memory_order_relaxed);
    }

    /// Called once, while walking the link section at boot. Ids are the
    /// section order, so they are stable for a given image and the exported
    /// name table is all the host needs to decode.
    auto assign_id(u16 id) const noexcept -> void {
        m_id = id;
    }

    [[nodiscard]]
    auto id() const noexcept -> u16 {
        return m_id;
    }

    /// A point in time.
    template <TraceArg... Args>
        requires(sizeof...(Args) <= MAX_TRACE_ARGS)
    auto emit(Args... args) const noexcept -> void {
        write(EventKind::Instant, args...);
    }

    /// The two halves of a region; prefer `Scope` over calling these, so a
    /// path that returns early cannot lose the `End`.
    template <TraceArg... Args>
        requires(sizeof...(Args) <= MAX_TRACE_ARGS)
    auto emit_begin(Args... args) const noexcept -> void {
        write(EventKind::Begin, args...);
    }

    auto emit_end() const noexcept -> void {
        write(EventKind::End);
    }

  private:
    template <TraceArg... Args>
    auto write(EventKind kind, Args... args) const noexcept -> void;

    char const *m_name;
    char const *m_category;
    /// Mutable so a `Tracepoint` can live in a read-only-ish const object in
    /// the link section while still being switched on at runtime.
    mutable std::atomic<bool> m_enabled{false};
    /// Assigned at boot by walking the section; 0 until then.
    mutable u16 m_id{0};
};

/// Records `Begin` on construction and `End` on destruction, so a region is
/// closed on every path out of the block -- including early returns, which
/// is the reason to prefer this over paired calls.
///
/// Scopes are lexical, not per-function: bracket a loop body, a lock hold,
/// or a single call just as easily as a whole function.
class TraceScope final {
  public:
    template <TraceArg... Args>
        requires(sizeof...(Args) <= MAX_TRACE_ARGS)
    explicit TraceScope(Tracepoint const &tracepoint, Args... args) noexcept
        // The decision is taken here and honoured in the destructor. Reading
        // `enabled()` again on the way out would let a site switched on
        // mid-region emit an `End` with no `Begin`, which desynchronises the
        // host's pairing.
        : m_tracepoint(tracepoint.enabled() ? &tracepoint : nullptr) {
        if (m_tracepoint != nullptr) {
            m_tracepoint->emit_begin(args...);
        }
    }

    ~TraceScope() {
        if (m_tracepoint != nullptr) {
            m_tracepoint->emit_end();
        }
    }

    TraceScope(TraceScope const &) = delete;
    auto operator=(TraceScope const &) -> TraceScope & = delete;

  private:
    Tracepoint const *m_tracepoint;
};

} // namespace prof

/// Declares a trace site. Only attribute macros are involved -- the site
/// itself is a plain object and the emit path a plain function, so it can be
/// named, scoped and stepped into like any other code.
///
///     PROF_DECLARE_TRACEPOINT(TP_SCHED_SWITCH, "sched_switch", "sched");
///
///     inline auto trace_sched_switch(u32 prev, u32 next) -> void {
///         TP_SCHED_SWITCH.emit(prev, next);
///     }
/// The section only exists in the kernel's own ELF link; examples and host
/// tests build without it and enumerate their tracepoints directly.
#if defined(__ELF__)
#define PROF_TRACEPOINT_SECTION LINK_SECTION(".prof.tracepoints")
#else
#define PROF_TRACEPOINT_SECTION
#endif

#define PROF_DECLARE_TRACEPOINT(SYMBOL, NAME, CATEGORY)                                            \
    PROF_TRACEPOINT_SECTION                                                                        \
    USED inline constinit ::prof::Tracepoint const SYMBOL {                                        \
        NAME, CATEGORY                                                                             \
    }

#endif // #ifndef PROF_TRACE_HPP
