/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_MEMORY_MODEL_HPP
#define OURS_MM_MEMORY_MODEL_HPP 1

#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/macro_abi.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm-frame.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>
#include <ustl/array.hpp>
#include <ustl/bitfields.hpp>
#include <ustl/limits.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/result.hpp>
#include <ustl/traits/conditional.hpp>
#include <ustl/traits/ptr.hpp>
#include <ustl/util/type-list.hpp>

#include <tuple>

namespace ours::mm {
namespace {
    template <typename T>
    FORCE_INLINE constexpr auto check_n_ptr(T p) -> bool {
        constexpr static auto kPtrDepth = ustl::traits::PtrDepthV<T>;

        if constexpr (kPtrDepth != 0) {
            return p && check_n_ptr(*p);
        } else {
            return true;
        }
    }

    template <usize N, typename T>
    FORCE_INLINE constexpr auto get_leaf_section(T *p)
        -> ustl::traits::RemovePtrDepthT<MemSection, N> {
        if constexpr (N != 0) {
            return get_leaf_section<N - 1>(*p);
        } else {
            return p;
        }
    }

    template <usize N>
    auto secnum_to_section(MemSection *root, SecNum secnum) -> MemSection * {
        using Pointer = ustl::traits::AddPtrDepthT<MemSection, N>;
        Pointer pointer = reinterpret_cast<Pointer>(root);
        if (!check_n_ptr<N>(pointer)) {
            return nullptr;
        }

        return get_leaf_section<N>(pointer);
    }

} // namespace

template <typename T>
FORCE_INLINE constexpr auto phys_to_secnum(T phys_addr) -> SecNum {
    static_assert(sizeof(T) == sizeof(PhysAddr));
    return reinterpret_cast<PhysAddr>(phys_addr) >> SECTION_SHIFT;
}

FORCE_INLINE constexpr auto secnum_to_phys(SecNum secnum) -> PhysAddr {
    return secnum << SECTION_SHIFT;
}

FORCE_INLINE constexpr auto pfn_to_secnum(Pfn pfn) -> SecNum {
    return pfn >> PFN_SECTION_SHIFT;
}

FORCE_INLINE constexpr auto secnum_to_pfn(SecNum secnum) -> Pfn {
    return secnum << PFN_SECTION_SHIFT;
}

class MemSection;

/// Leaf node
using MemMap = ustl::views::Span<PmFrame>;

/// Non-leaf node
using SecMap = ustl::views::Span<MemSection>;

// [Internal]
class MemSection {
    using Self = MemSection;

  public:
    enum class State {
        None,
        Present,
        Online,
        Count,
    };

    enum class Type {
        Unknown,
        Terminal,
        Interminal,
        Count,
    };

    constexpr static auto const kMaxLevel = 3;

    FORCE_INLINE constexpr auto state() const -> State {
        return m_value.get<Index::State>();
    }

    FORCE_INLINE constexpr auto set_state(State state) -> void {
        m_value.set<Index::State>(state);
    }

    FORCE_INLINE constexpr auto type() const -> Type {
        return m_value.get<Index::Type>();
    }

    FORCE_INLINE constexpr auto set_type(Type type) -> void {
        m_value.set<Index::Type>(type);
    }

    FORCE_INLINE constexpr auto level() const -> usize {
        return m_value.get<Index::Level>();
    }

    FORCE_INLINE constexpr auto set_level(usize level) -> void {
        m_value.set<Index::Level>(level);
    }

    FORCE_INLINE constexpr auto set_state_present() -> void {
        m_value.set<Index::State>(State::Present);
    }

    FORCE_INLINE constexpr auto set_state_online() -> void {
        m_value.set<Index::State>(State::Online);
    }

    FORCE_INLINE constexpr auto mark_terminal() -> void {
        m_value.set<Index::Type>(Type::Terminal);
    }

    FORCE_INLINE constexpr auto mark_interminal() -> void {
        m_value.set<Index::Type>(Type::Interminal);
    }

    FORCE_INLINE constexpr auto memmap() const -> MemMap {
        DEBUG_ASSERT(is_termianl());
        return {static_cast<PmFrame *>(m_value.get<Index::Map>()), PAGE_SIZE / sizeof(MemMap *)};
    }

    FORCE_INLINE constexpr auto set_memmap(PmFrame *map) -> void {
        DEBUG_ASSERT(is_termianl());
        m_value.set<Index::Map>(map);
    }

    FORCE_INLINE constexpr auto secmap() const -> SecMap {
        DEBUG_ASSERT(is_termianl() == false);
        return {static_cast<Self *>(m_value.get<Index::Map>()), PAGE_SIZE / sizeof(*this)};
    }

    FORCE_INLINE constexpr auto set_secmap(Self *map) -> void {
        DEBUG_ASSERT(is_termianl() == false);
        m_value.set<Index::Map>(map);
    }

    FORCE_INLINE constexpr auto has_map() const -> bool {
        return m_value.get<Index::Map>() == nullptr;
    }

    FORCE_INLINE constexpr auto is_state_present() const -> bool {
        return state() == State::Present;
    }

    FORCE_INLINE constexpr auto is_state_online() const -> bool {
        return state() == State::Online;
    }

    FORCE_INLINE constexpr auto is_termianl() const -> bool {
        return type() == Type::Terminal;
    }

  private:
    enum class Index {
        State,
        Type,
        Level,
        Map,
    };

    template <Index Id, typename Type, usize Bits, bool Natural = false>
    using Field = ustl::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Type<Type>,
                              ustl::bitfields::Bits<Bits>, ustl::bitfields::Natural<Natural>>;

    using Value = ustl::BitFields<
        ustl::bitfields::StorageUnit<usize>,
        std::tuple<Field<Index::State, State, ustl::bit_width(static_cast<usize>(State::Count))>,
                   Field<Index::Type, Type, ustl::bit_width(static_cast<usize>(Type::Count))>,
                   Field<Index::Level, usize, ustl::bit_width<usize>(kMaxLevel)>,
                   // FIXME(SmallHuaZi): Hard code.
                   Field<Index::Map, void *, 58, true>>>;
    Value m_value;
};

/// `MemoryModel` is a high-level abstract and implementation to physical memory layout.
/// It's responsibility is to pre-allocate `PmFrame` and to provide a group of the convertion
/// method among `PmFrame`, `Pfn`, `SecNum`, `VirtAddr` and `PhysAddr`.
class MemoryModel {
    using Self = MemoryModel;

  public:
    constexpr static usize const kMappingLevel = MemSection::kMaxLevel;

    constexpr static usize const kTopLevel = kMappingLevel - 1;

    constexpr static usize const kEntriesPerSection = PAGE_SIZE / sizeof(MemSection);

    constexpr static usize const kFramesPerSection = BIT(SECTION_SHIFT - PAGE_SHIFT);

    constexpr static auto const kLeavesPerLevel =
        []<usize... Levels>(ustl::IndexSequence<Levels...>) {
            static_assert(sizeof...(Levels) == kMappingLevel);
            ustl::Array<usize, sizeof...(Levels)> array;
            // L0 entry is a leaf itself.
            array[0] = 1;
            for (usize i = 1; i < array.size(); ++i) {
                array[i] = array[i - 1] * kEntriesPerSection;
            }
            return array;
        }(ustl::MakeIndexSequenceT<kMappingLevel>());

    constexpr static auto const kFramesPerLevel =
        []<usize... Levels>(ustl::IndexSequence<Levels...>) {
            static_assert(sizeof...(Levels) == kMappingLevel);
            ustl::Array<usize, sizeof...(Levels)> array;
            array[0] = BIT(SECTION_SHIFT - PAGE_SHIFT);
            for (usize i = 1; i < array.size(); ++i) {
                array[i] = array[i - 1] * kEntriesPerSection;
            }
            return array;
        }(ustl::MakeIndexSequenceT<kMappingLevel>());

    constexpr static usize const kIndexBits = {
        (ustl::NumericLimits<usize>::DIGITS - SECTION_SIZE_BITS + kMappingLevel) / kMappingLevel};

    FORCE_INLINE
    static auto get() -> Self & {
        static Self g_memory_model;
        return g_memory_model;
    }

    INIT_CODE
    auto init(bool map_init) -> Status;

    INIT_CODE
    auto init_memmap() -> void;

    /// Estimate whether the frame descriptor of given pfn exist. It is do not means that
    /// the memory that descriptor represents is available (may hole).
    auto exist(Pfn pfn) const -> bool {
        auto section = pfn_to_section(pfn);
        if (!section) {
            return false;
        }

        return section->is_state_present();
    }

    auto exist(Pfn start, Pfn end) const -> bool;

    FORCE_INLINE constexpr auto pfn_to_section(Pfn pfn) const -> MemSection * {
        return secnum_to_section(pfn_to_secnum(pfn));
    }

    FORCE_INLINE constexpr auto secnum_to_section(SecNum secnum) const -> MemSection * {
        return priv_secnum_to_section(secnum);
    }

    FORCE_INLINE constexpr auto pfn_to_frame(Pfn pfn) -> PmFrame * {
        if (auto const section = secnum_to_section(pfn_to_secnum(pfn))) {
            return ustl::mem::address_of(section->memmap()[pfn & PFN_SECTION_MASK]);
        }

        return nullptr;
    }

    FORCE_INLINE constexpr auto frame_to_pfn(PmFrame const *frame) -> Pfn {
        DEBUG_ASSERT(frame != nullptr);
        if (auto const section = frame_to_section(frame)) {
            return secnum_to_pfn(frame->secnum()) + (frame - section->memmap().data());
        }

        return 0;
    }

    FORCE_INLINE constexpr auto frame_to_section(PmFrame const *frame) -> MemSection * {
        DEBUG_ASSERT(frame != nullptr);
        return secnum_to_section(frame->secnum());
    }

    auto add_range(PhysAddr start, PhysAddr end, NnId nid) -> Status;

    auto remove_range(PhysAddr start, PhysAddr end, NnId nid) -> Status;

    auto dump() -> void;

  private:
    class Impl;

    MemoryModel() = default;
    ~MemoryModel() = default;

    USTL_NOT_COPYABLE_AND_MOVEABLE(MemoryModel);

    template <typename F>
    auto descend(SecNum start, SecNum end, F &&f) -> Status;

    auto init_index(usize level, SecNum begin, NnId nid) -> ustl::Result<MemSection *, Status>;

    FORCE_INLINE
    auto activate_section(SecNum secnum, NnId nid) -> ustl::Result<MemMap, Status> {
        auto section = secnum_to_section(secnum);
        if (!section) {
            return ustl::err(Status::OutOfMem);
        }

        return activate_section(section, nid);
    }

    /// Allocate memmap for the given section and mark it active.
    auto activate_section(MemSection *section, NnId nid) -> ustl::Result<MemMap, Status>;

    /// Initialize the page frames in the overlapping regions between [ `|start_pfn|`, `|end_pfn|`) and
    /// the `|zone|` managed memory regions.
    ///
    /// Return the pfn of the first frame which not be initialized.
    INIT_CODE
    auto init_memmap_zone(ZoneType zone, Pfn zone_start, Pfn zone_end, Pfn start, Pfn end,
                          Pfn hole_start, NnId nid) -> Pfn;

    INIT_CODE
    auto init_memmap_nid(ZoneType zone, Pfn zone_start, Pfn zone_end, Pfn start, Pfn end,
                         Pfn hole_start, NnId nid) -> Pfn;

    INIT_CODE
    auto init_memmap_range(Pfn start_pfn, Pfn end_pfn, ZoneType zone, NnId nid) -> void;

    INIT_CODE
    auto init_memmap_reserved(Pfn start_pfn, Pfn end_pfn, ZoneType ztype, NnId nid) -> void;

    MEM_INIT_CODE
    auto populate_memmap(Pfn start, usize nr_frames, NnId nid) -> ustl::Result<MemMap, Status>;

    auto depopulate_memmap(Pfn start, usize nr_frames, NnId nid) -> Status;

    auto mark_section(SecNum begin, SecNum end, MemSection::State) -> void;

    FORCE_INLINE constexpr static auto secnum_to_index(usize level, SecNum secnum) -> usize {
        constinit static auto const kShift = []() {
            ustl::Array<usize, kMappingLevel> array;
            array[0] = 0;
            for (usize i = 1; i < array.size(); ++i) {
                array[i] = array[i - 1] + kIndexBits;
            }
            return array;
        }();

        return (secnum >> kShift[level]) & (BIT(kIndexBits) - 1);
    }

    FORCE_INLINE constexpr auto priv_secnum_to_section(SecNum secnum) const -> MemSection * {
        DEBUG_ASSERT(m_root_section);

        /// A short loop will be unfold.
        MemSection *section = m_root_section;
        for (isize level = kMappingLevel - 1; level >= 0; --level) {
            section = ustl::mem::address_of(section[secnum_to_index(level, secnum)]);
            if (!section->is_state_present()) {
                return nullptr;
            }
            if (section->is_termianl()) {
                return section;
            }
            section = section->secmap().data();
        }

        return section;
    }

    MemSection *m_root_section;
};

/// Accessors for conveniency.

FORCE_INLINE constexpr auto pfn_to_frame(Pfn pfn) -> PmFrame * {
    return MemoryModel::get().pfn_to_frame(pfn);
}

template <typename T>
FORCE_INLINE constexpr auto phys_to_frame(T phys_addr) -> PmFrame * {
    return pfn_to_frame(phys_to_pfn(phys_addr));
}

FORCE_INLINE constexpr auto frame_to_pfn(PmFrame const *frame) -> Pfn {
    return MemoryModel::get().frame_to_pfn(frame);
}

FORCE_INLINE constexpr auto frame_to_phys(PageFrame const *frame) -> PhysAddr {
    return pfn_to_phys(frame_to_pfn(frame->as_ptr<PmFrame>()));
}

template <typename T>
FORCE_INLINE constexpr auto virt_to_frame(T virt_addr) -> PmFrame * {
    return phys_to_frame(PhysMap::virt_to_phys(virt_addr));
}

FORCE_INLINE constexpr auto frame_to_virt(PmFrame *frame) -> VirtAddr {
    return PhysMap::phys_to_virt(frame_to_phys(frame));
}

template <typename T>
FORCE_INLINE constexpr auto frame_to_virt(PmFrame *frame) -> T * {
    DEBUG_ASSERT(frame->phys_size() >= sizeof(T), "T is too large");
    return PhysMap::phys_to_virt<T>(frame_to_phys(frame));
}

template <typename T>
FORCE_INLINE constexpr auto virt_to_folio(T virt_addr) -> PmFolio * {
    return frame_to_folio(phys_to_frame(PhysMap::virt_to_phys(virt_addr)));
}

} // namespace ours::mm

#endif // #ifndef OURS_MM_memory-model_HPP
