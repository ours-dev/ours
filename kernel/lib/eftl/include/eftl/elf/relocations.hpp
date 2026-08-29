/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_RELOC_ENTRYATIONS_HPP
#define EFTL_RELOC_ENTRYATIONS_HPP 1

#include <eftl/elf/config.hpp>
#include <eftl/elf/dynamic.hpp>
#include <eftl/elf/observable.hpp>
#include <eftl/elf/traits.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>

#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
template <typename Traits>
struct Relocations final: Observable<Traits, Relocations<Traits>> {
    using Self = Relocations;
    using Addr = typename Traits::Addr;
    using Rel = typename Traits::Rel;
    using Rela = typename Traits::Rela;
    using SAddr = std::make_signed_t<Addr>;

    using RelrTable = ustl::views::Span<Addr const>;
    using RelTable = ustl::views::Span<Rel const>;
    using RelaTable = ustl::views::Span<Rela const>;

    // These names describe the public relocation record.
    // NOLINTBEGIN(readability-identifier-naming)
    struct RelativeRelocation {
        Addr offset;
        SAddr addend;
        bool addend_in_place;
    };
    // NOLINTEND(readability-identifier-naming)

    FORCE_INLINE [[nodiscard]]
    constexpr auto relative_reltable() -> RelTable {
        return m_rel_table.subspan(0, m_rel_count);
    }

    FORCE_INLINE [[nodiscard]]
    constexpr auto relative_relatable() -> RelaTable {
        return m_rela_table.subspan(0, m_rela_count);
    }

    template <typename Visitor>
    [[nodiscard]]
    auto visit_relative(Visitor &&visitor) -> bool {
        if (!relative_reltable().empty() || !relative_relatable().empty()) {
            return false;
        }
        return visit_relr_table([&visitor](Addr offset) {
            return visitor(RelativeRelocation{
                .offset = offset,
                .addend = 0,
                .addend_in_place = true,
            });
        });
    }

    template <typename IsRelative, typename Visitor>
    [[nodiscard]]
    auto visit_relative(IsRelative &&is_relative, Visitor &&visitor) -> bool {
        for (auto const &rel: relative_reltable()) {
            if (Traits::relocation_symbol(rel.r_info) != 0 ||
                !is_relative(Traits::relocation_type(rel.r_info)) ||
                !visitor(RelativeRelocation{
                    .offset = static_cast<Addr>(rel.r_offset),
                    .addend = 0,
                    .addend_in_place = true,
                })) {
                return false;
            }
        }
        for (auto const &rela: relative_relatable()) {
            if (Traits::relocation_symbol(rela.r_info) != 0 ||
                !is_relative(Traits::relocation_type(rela.r_info)) ||
                !visitor(RelativeRelocation{
                    .offset = static_cast<Addr>(rela.r_offset),
                    .addend = static_cast<SAddr>(rela.r_addend),
                    .addend_in_place = false,
                })) {
                return false;
            }
        }
        return visit_relr_table([&visitor](Addr offset) {
            return visitor(RelativeRelocation{
                .offset = offset,
                .addend = 0,
                .addend_in_place = true,
            });
        });
    }

    FORCE_INLINE constexpr auto set_rel(RelTable rel, usize count) -> Self & {
        m_rel_table = rel;
        m_rel_count = std::min(count, rel.size());
        return *this;
    }

    FORCE_INLINE constexpr auto set_rela(RelaTable rela, usize count) -> Self & {
        m_rela_table = rela;
        m_rela_count = std::min(count, m_rela_table.size());
        return *this;
    }

    FORCE_INLINE constexpr auto set_relr(RelrTable relr) -> Self & {
        m_relr_table = relr;
        return *this;
    }

  private:
    template <typename Visitor>
    [[nodiscard]]
    constexpr auto visit_relr_table(Visitor &&visitor) -> bool {
        // In the .relr.dyn section,
        //  1) An even entry indicates a location which needs a relocation
        //     and sets up where for subsequent odd entries.
        //  2) An odd entry indicates a bitmap encoding up to 63 locations
        //     following where.
        //  3) Odd entries can be chained.

        auto const address_limit = std::numeric_limits<Addr>::max();
        auto const address_size = static_cast<Addr>(sizeof(Addr));
        auto const bitmap_bits = static_cast<usize>(std::numeric_limits<Addr>::digits - 1);
        auto const bitmap_stride = static_cast<Addr>(bitmap_bits * sizeof(Addr));

        Addr where = 0;
        for (auto const &entry: m_relr_table) {
            if ((entry & 1) == 0) {
                if (entry > address_limit - address_size) {
                    return false;
                }
                auto const next_where = entry + address_size;
                if (!visitor(entry)) {
                    return false;
                }
                where = next_where;
                continue;
            }

            if (where > address_limit - bitmap_stride) {
                return false;
            }
            auto const next_where = where + bitmap_stride;
            auto const bitmap = entry >> 1;
            for (usize bit = 0; bit < bitmap_bits; ++bit) {
                if ((bitmap & (Addr(1) << bit)) != 0) {
                    auto const relative_offset = static_cast<Addr>(bit * sizeof(Addr));
                    if (where > address_limit - relative_offset) {
                        return false;
                    }
                    auto const target = where + relative_offset;
                    if (!visitor(target)) {
                        return false;
                    }
                }
            }
            where = next_where;
        }

        return true;
    }

    usize m_rel_count{};
    RelTable m_rel_table;
    usize m_rela_count{};
    RelaTable m_rela_table;
    RelrTable m_relr_table;
};

using RelTagToIndexMap = ustl::TypeMap<
    ustl::TypeMapEntry<DynTagMatcher<DynTag::Rel>, ustl::traits::IntConstant<0>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::RelSz>, ustl::traits::IntConstant<1>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::RelCount>, ustl::traits::IntConstant<2>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::Rela>, ustl::traits::IntConstant<3>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::RelaSz>, ustl::traits::IntConstant<4>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::RelaCount>, ustl::traits::IntConstant<5>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::Relr>, ustl::traits::IntConstant<6>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::RelrSz>, ustl::traits::IntConstant<7>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::PltRel>, ustl::traits::IntConstant<8>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::PltRelSz>, ustl::traits::IntConstant<9>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::JmpRel>, ustl::traits::IntConstant<10>>>;

template <typename Traits>
struct RelocationObserver;

template <typename Traits>
using RelocationObserverBase =
    DynInfoObserver<RelocationObserver<Traits>, Relocations<Traits>, Traits, RelTagToIndexMap,
                    DynTag::Rel, DynTag::RelSz, DynTag::Rela, DynTag::RelaSz, DynTag::Relr,
                    DynTag::RelrSz, DynTag::PltRel, DynTag::PltRelSz, DynTag::JmpRel,
                    DynTag::RelCount, DynTag::RelaCount, DynTag::RelEnt, DynTag::RelaEnt,
                    DynTag::RelrEnt>;

template <typename Traits>
struct RelocationObserver final: public RelocationObserverBase<Traits> {
    using Base = RelocationObserverBase<Traits>;
    using InfoType = typename Base::InfoType;
    using Addr = typename Traits::Addr;
    using Rel = typename Traits::Rel;
    using Rela = typename Traits::Rela;
    using Relr = Addr;

    using Base::Base;
    using Base::observe;
    using Base::write_back;

    template <typename Memory, DynTag Tag>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &&memory, Addr val, DynTagMatcher<Tag> tag) -> bool {
        if constexpr (Tag == DynTag::RelEnt) {
            return val == sizeof(Rel);
        } else if constexpr (Tag == DynTag::RelaEnt) {
            return val == sizeof(Rela);
        } else if constexpr (Tag == DynTag::RelrEnt) {
            return val == sizeof(Relr);
        } else {
            return Base::observe(memory, val, tag);
        }
    }

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto finish(Memory &&memory) -> bool {
        // PLT/symbolic relocations need a symbol and string-table resolver. Reject them instead
        // of silently ignoring a table that the relative-only loader cannot apply.
        if (Base::template has<DynTag::JmpRel>() || Base::template has<DynTag::PltRelSz>() ||
            Base::template has<DynTag::PltRel>()) {
            return false;
        }

        return {Base::template write_back<Rel, &InfoType::set_rel, DynTag::Rel, DynTag::RelSz,
                                          DynTag::RelCount>(memory) &&
                Base::template write_back<Rela, &InfoType::set_rela, DynTag::Rela, DynTag::RelaSz,
                                          DynTag::RelaCount>(memory) &&
                Base::template write_back<Relr, &InfoType::set_relr, DynTag::Relr, DynTag::RelrSz>(
                    memory)};
    }
};

template <typename Traits>
struct InitFiniInfo final: Observable<Traits, InitFiniInfo<Traits>> {
    using Self = InitFiniInfo;
    using Addr = typename Traits::Addr;
    using InitFiniArray = ustl::views::Span<Addr>;

    FORCE_INLINE constexpr auto set_array(InitFiniArray array) -> Self & {
        array_ = array;
        return *this;
    }

    FORCE_INLINE constexpr auto set_legacy(Addr val) -> Self & {
        legacy_ = val;
        return *this;
    }

    template <typename Visitor>
    FORCE_INLINE [[nodiscard]]
    constexpr auto visit(Visitor &&visitor) -> bool {
        for (auto &item: array_) {
            if (!visitor(item)) {
                return false;
            }
        }

        return true;
    }

    // NOLINTBEGIN(readability-identifier-naming)
    Addr legacy_;
    InitFiniArray array_;
    // NOLINTEND(readability-identifier-naming)
};

template <typename Traits, DynTag Array, DynTag ArraySz, DynTag Legacy>
struct DynInitFiniObserver;

using InitFiniTagToIndexMap = ustl::TypeMap<
    ustl::TypeMapEntry<DynTagMatcher<DynTag::InitArray>, ustl::traits::IntConstant<0>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::InitArraySz>, ustl::traits::IntConstant<1>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::FiniArray>, ustl::traits::IntConstant<0>>,
    ustl::TypeMapEntry<DynTagMatcher<DynTag::FiniArraySz>, ustl::traits::IntConstant<1>>>;

// This is just a shorthand to avoid repeating the long list of parameters.
template <typename Traits, DynTag Array, DynTag ArraySz, DynTag Legacy>
using DynInitFiniObserverBase =
    DynInfoObserver<DynInitFiniObserver<Traits, Array, ArraySz, Legacy>, InitFiniInfo<Traits>,
                    Traits, InitFiniTagToIndexMap, Array, ArraySz, Legacy>;

template <typename Traits, DynTag Array, DynTag ArraySz, DynTag Legacy>
struct DynInitFiniObserver final: public DynInitFiniObserverBase<Traits, Array, ArraySz, Legacy> {
    using Base = DynInitFiniObserverBase<Traits, Array, ArraySz, Legacy>;
    using Info = InitFiniInfo<Traits>;
    using Addr = typename Traits::Addr;
    using InfoType = typename Base::InfoType;

    using Base::Base;
    using Base::observe;

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &memory, Addr val, DynTagMatcher<Legacy>) -> bool {
        (void)memory;
        this->info().set_legacy(val);
        return true;
    }

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto finish(Memory &&memory) -> bool {
        return Base::template write_back<Addr, &InfoType::set_array, Array, ArraySz>(memory);
    }
};

template <typename Traits>
using DynInitObserver =
    DynInitFiniObserver<Traits, DynTag::InitArray, DynTag::InitArraySz, DynTag::Init>;

template <typename Traits>
using DynFiniObserver =
    DynInitFiniObserver<Traits, DynTag::FiniArray, DynTag::FiniArraySz, DynTag::Fini>;

template <typename Traits>
struct StrTableInfo final: public Observable<Traits, StrTableInfo<Traits>> {
    using Self = StrTableInfo;
    using StrTable = ustl::views::Span<u8 const>;

    FORCE_INLINE constexpr auto set_str_table(StrTable array) -> Self & {
        strtable_ = array;
        return *this;
    }

    template <typename Visitor>
    FORCE_INLINE [[nodiscard]]
    constexpr auto visit(Visitor &&visitor) -> bool {
        for (auto &item: strtable_) {
            if (!visitor(item)) {
                return false;
            }
        }
        return true;
    }

    // NOLINTBEGIN(readability-identifier-naming)
    StrTable strtable_;
    // NOLINTEND(readability-identifier-naming)
};

using DynStrTagToIndexMap =
    ustl::TypeMap<ustl::TypeMapEntry<DynTagMatcher<DynTag::StrTab>, ustl::traits::IntConstant<0>>,
                  ustl::TypeMapEntry<DynTagMatcher<DynTag::StrSz>, ustl::traits::IntConstant<1>>>;

template <typename Traits>
struct DynStrObserver;

template <typename Traits>
using DynStrObserverBase = DynInfoObserver<DynStrObserver<Traits>, StrTableInfo<Traits>, Traits,
                                           DynStrTagToIndexMap, DynTag::StrTab, DynTag::StrSz>;

template <typename Traits>
struct DynStrObserver final: public DynStrObserverBase<Traits> {
    using Base = DynStrObserverBase<Traits>;
    using InfoType = typename Base::InfoType;

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto finish(Memory &&memory) -> bool {
        return Base::template write_back<u8, &InfoType::set_str_table, DynTag::StrTab,
                                         DynTag::StrSz>(memory);
    }
};

} // namespace eftl::elf

#endif // #ifndef EFTL_RELOC_ENTRYATIONS_HPP
