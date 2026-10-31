/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_TABLE_SET_HPP
#define ACPI_TABLE_SET_HPP 1

#include <acpi/structures/fadt.hpp>
#include <acpi/structures/header.hpp>
#include <acpi/structures/root.hpp>
#include <acpi/types.hpp>

#include <array>

#include <ktl/result.hpp>

namespace acpi {
/// Turns a physical address into something the caller can read.
///
/// Two adapters exist and they differ in kind, which is what makes this a real
/// seam rather than a hypothetical one: //kernel/phys runs identity mapped and
/// returns the address unchanged, while the kernel proper goes through
/// mem::PhysMap.
///
/// Contract, which callers and adapters both depend on:
///   - On success the whole range [addr, addr + size) is readable.
///   - The returned mapping stays valid for the lifetime of the program.  There
///     is no unmap; adapters that cannot promise this cannot be used here.
///   - `map()` may be called several times for overlapping ranges, and must
///     return consistent pointers when it does.
class PhysMapper {
  public:
    virtual auto map(PhysAddr addr, usize size) -> ktl::Result<void const *> = 0;

  protected:
    PhysMapper() = default;
    virtual ~PhysMapper() = default;
};

auto calc_checksum(void const *buffer, usize len) -> u8;

[[nodiscard]]
inline auto validate_checksum(void const *buffer, usize len) -> bool {
    return 0 == calc_checksum(buffer, len);
}

/// The set of ACPI tables reachable from the RSDP.
///
/// Tables are mapped lazily: constructing only resolves the RSDP and the root
/// table, and each description table is mapped the first time something asks
/// for it.  Successful signature lookups are memoised, so the O(n) walk over
/// the root table happens at most once per signature instead of once per call
/// -- the old parser re-walked and re-mapped everything on every query, which
/// meant seven full scans just to bring up APIC and NUMA.
///
/// Not thread safe: `find()` and `header_at()` mutate the lookup cache.  Every
/// caller today runs during single threaded platform init.
class TableSet final {
    using Self = TableSet;

  public:
    /// Number of description tables the root table claims.  Individual entries
    /// may still fail to map or validate.
    [[nodiscard]]
    auto num_tables() const -> usize {
        return m_num_tables;
    }

    /// The only supported way to reach a typed table.
    ///
    /// Checks the signature, that the table is at least `sizeof(T)` bytes, and
    /// the checksum -- all in one place.  Callers that hand-rolled this used to
    /// forget the length check, which is how a truncated SLIT could be read
    /// eight bytes past the end of its mapping.
    template <typename T>
    [[nodiscard]]
    auto find() -> T const * {
        return reinterpret_cast<T const *>(find_checked(T::SIGNATURE, sizeof(T)));
    }

    /// The DSDT, or nullptr when there is none that can be used.
    ///
    /// The root table does not list it: the FADT points to it, by the 64 bit
    /// `x_dsdt` when the FADT is long enough to have one and it is not zero,
    /// otherwise by the 32 bit `dsdt`.  Checks the signature and the checksum,
    /// as `find()` does, and is not memoised: the walk is one lookup and one
    /// mapping.
    [[nodiscard]]
    auto dsdt() -> TableHeader const *;

    /// Indexed access to the raw headers, for diagnostics that want to list
    /// what firmware provided.  Maps on demand like `find()` does.
    [[nodiscard]]
    auto header_at(usize index) -> ktl::Result<TableHeader const *>;

    [[nodiscard]]
    auto phys_addr_at(usize index) const -> PhysAddr;

    static auto from_rsdp(PhysMapper &mapper, PhysAddr rsdp) -> ktl::Result<TableSet>;

  private:
    /// Private, and there is no public default constructor: the previous parser
    /// had one, left every member uninitialised, and answered `num_tables()`
    /// with whatever was on the stack.
    TableSet() = default;

    FORCE_INLINE
    constexpr TableSet(PhysMapper *mapper, sdt::Rsdt const *rsdt, sdt::Xsdt const *xsdt,
                       usize num_tables)
        : m_mapper(mapper), m_rsdt(rsdt), m_xsdt(xsdt), m_num_tables(num_tables) {}

    auto find_checked(Signature sign, usize min_size) -> TableHeader const *;

    auto scan_for(Signature sign) -> TableHeader const *;

    /// Memoises one signature lookup.  A `header` of nullptr records a table
    /// that is known to be absent or malformed, so repeated misses stay cheap
    /// -- and a negative result is re-scanned only if the cache overflowed.
    struct CacheSlot {
        Signature sign;
        TableHeader const *header;
    };

    /// Sized for the handful of signatures the kernel actually asks for (APIC,
    /// SRAT, SLIT, HPET, DBG2, ...).  Overflowing the cache costs a rescan, not
    /// a wrong answer, so there is no eviction policy to get wrong.
    constexpr static usize CACHE_SLOTS = 8;

    [[nodiscard]]
    auto cached(Signature sign) const -> CacheSlot const *;

    auto remember(Signature sign, TableHeader const *header) -> void;

    PhysMapper *m_mapper{nullptr};
    sdt::Rsdt const *m_rsdt{nullptr};
    sdt::Xsdt const *m_xsdt{nullptr};
    usize m_num_tables{0};
    std::array<CacheSlot, CACHE_SLOTS> m_cache{};
    usize m_cache_len{0};
};

/// Iterates the sub-entries of `Table` that have `Entry`'s type tag.
///
/// This replaces the six near-identical "look the table up, build a reader,
/// loop, downcast, invoke a type-erased callback" functions the library used to
/// expose -- one per entry type, each with its own `...CommitFn` typedef, and
/// none of them honouring the Status their callback returned.  A range lets the
/// caller `break`, keeps the concrete entry type, and costs no indirection.
///
/// Walking stops at the first malformed entry; `malformed()` reports whether
/// that happened, so callers who care can tell truncation from a clean end.
template <typename Entry>
class EntryRange {
    using Self = EntryRange;

  public:
    class Iterator {
      public:
        [[nodiscard]]
        auto operator*() const -> Entry const & {
            return *reinterpret_cast<Entry const *>(m_current);
        }

        auto operator++() -> Iterator & {
            m_current += reinterpret_cast<EntryHeader const *>(m_current)->length;
            advance_to_match();
            return *this;
        }

        [[nodiscard]]
        friend auto operator==(Iterator const &x, Iterator const &y) -> bool {
            return x.m_current == y.m_current;
        }

        Iterator() = default;

      private:
        friend Self;

        Iterator(u8 const *current, u8 const *end, bool *malformed)
            : m_current(current), m_end(end), m_malformed(malformed) {
            advance_to_match();
        }

        /// Walks forward until `m_current` is an entry of the wanted type, or
        /// until the payload ends.  Any entry that cannot be trusted -- one
        /// whose header does not fit, whose length would run past the end, or
        /// whose length is below the two byte minimum -- ends the walk.
        auto advance_to_match() -> void {
            while (m_current < m_end) {
                if (usize(m_end - m_current) < sizeof(EntryHeader)) {
                    break;
                }

                auto const *header = reinterpret_cast<EntryHeader const *>(m_current);
                auto const length = usize(header->length);
                if (length < sizeof(EntryHeader) || length > usize(m_end - m_current)) {
                    break;
                }
                if (header->type == static_cast<u8>(Entry::TYPE)) {
                    // A well formed entry of the right type must still be long
                    // enough to hold the fields we are about to read.
                    if (length < sizeof(Entry)) {
                        break;
                    }
                    return;
                }

                m_current += length;
            }

            if (m_current != m_end) {
                *m_malformed = true;
            }
            m_current = m_end;
        }

        u8 const *m_current{nullptr};
        u8 const *m_end{nullptr};
        bool *m_malformed{nullptr};
    };

    template <typename Table>
    explicit EntryRange(Table const &table)
        : m_begin(reinterpret_cast<u8 const *>(&table) + sizeof(Table))
        , m_end(reinterpret_cast<u8 const *>(&table) + table.size()) {
        // `find<Table>()` already guaranteed the table is at least sizeof(Table)
        // bytes, so this cannot run backwards; be defensive anyway, since the
        // range is also constructible from a table the caller assembled itself.
        if (m_end < m_begin) {
            m_end = m_begin;
        }
    }

    [[nodiscard]]
    auto begin() const -> Iterator {
        return {m_begin, m_end, &m_malformed};
    }

    [[nodiscard]]
    auto end() const -> Iterator {
        return {m_end, m_end, &m_malformed};
    }

    /// True if the walk stopped early because an entry did not fit.  Only
    /// meaningful after iterating.
    [[nodiscard]]
    auto malformed() const -> bool {
        return m_malformed;
    }

  private:
    u8 const *m_begin;
    u8 const *m_end;
    /// Written through the iterators' pointer when a walk stops early. The
    /// range's one yes/no fact, so it has no word to share; mutable because
    /// begin() and end() are const and the walk is not a change to the range.
    mutable bool m_malformed{false};
};

/// Convenience so callers write `acpi::entries<madt::IoApicEntry>(*madt)`.
template <typename Entry, typename Table>
[[nodiscard]]
auto entries(Table const &table) -> EntryRange<Entry> {
    return EntryRange<Entry>(table);
}

} // namespace acpi

#endif // #ifndef ACPI_TABLE_SET_HPP
