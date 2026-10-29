/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_SPARSE_MEMMAP_HPP
#define OURS_MM_PRIV_SPARSE_MEMMAP_HPP 1

#include <ours/assert.hpp>
#include <ours/init.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/priv/memmap.hpp>
#include <ours/mm/types.hpp>
#include <ours/status.hpp>

#include <array>
#include <tuple>
#include <utility>

#include <ustl/bitfields.hpp>
#include <ustl/limits.hpp>
#include <ustl/mem/address_of.hpp>
#include <ustl/result.hpp>
#include <ustl/traits/conditional.hpp>
#include <ustl/traits/ptr.hpp>
#include <ustl/util/type_list.hpp>

namespace ours::mm::priv {
namespace {
    template <typename T>
    FORCE_INLINE
    constexpr auto check_n_ptr(T p) -> bool {
        constexpr static auto PTR_DEPTH = ustl::traits::PtrDepthV<T>;

        if constexpr (PTR_DEPTH != 0) {
            return p && check_n_ptr(*p);
        } else {
            return true;
        }
    }

    template <usize N, typename T>
    FORCE_INLINE
    constexpr auto get_leaf_section(T *p) -> ustl::traits::RemovePtrDepthT<MemSection, N> {
        if constexpr (N != 0) {
            return get_leaf_section<N - 1>(*p);
        } else {
            return p;
        }
    }

    template <usize N>
    [[maybe_unused]]
    auto secnum_to_section(MemSection *root, SecNum secnum) -> MemSection * {
        using Pointer = ustl::traits::AddPtrDepthT<MemSection, N>;
        Pointer pointer = reinterpret_cast<Pointer>(root);
        if (!check_n_ptr<N>(pointer)) {
            return nullptr;
        }

        return get_leaf_section<N>(pointer);
    }

} // namespace

class MemSection;

/// Non-leaf node
using SecMap = ustl::views::Span<MemSection>;

// [Internal]
class MemSection {
    using Self = MemSection;

  public:
    enum class State : u32 {
        None,
        Present,
        Online,
        Count,
    };

    enum class Type : u32 {
        Unknown,
        Terminal,
        Interminal,
        Count,
    };

    constexpr static auto MAX_LEVEL = 3;

    FORCE_INLINE
    constexpr auto state() const -> State {
        return m_value.get<Index::State>();
    }

    FORCE_INLINE
    constexpr auto set_state(State state) -> void {
        m_value.set<Index::State>(state);
    }

    FORCE_INLINE
    constexpr auto type() const -> Type {
        return m_value.get<Index::Type>();
    }

    FORCE_INLINE
    constexpr auto set_type(Type type) -> void {
        m_value.set<Index::Type>(type);
    }

    FORCE_INLINE
    constexpr auto level() const -> usize {
        return m_value.get<Index::Level>();
    }

    FORCE_INLINE
    constexpr auto set_level(usize level) -> void {
        m_value.set<Index::Level>(level);
    }

    FORCE_INLINE
    constexpr auto set_state_present() -> void {
        m_value.set<Index::State>(State::Present);
    }

    FORCE_INLINE
    constexpr auto set_state_online() -> void {
        m_value.set<Index::State>(State::Online);
    }

    FORCE_INLINE
    constexpr auto mark_terminal() -> void {
        m_value.set<Index::Type>(Type::Terminal);
    }

    FORCE_INLINE
    constexpr auto mark_interminal() -> void {
        m_value.set<Index::Type>(Type::Interminal);
    }

    FORCE_INLINE
    constexpr auto memmap() const -> FrameSpan {
        DEBUG_ASSERT(is_termianl());
        return {static_cast<PmFrame *>(m_value.get<Index::Map>()), PAGE_SIZE / sizeof(FrameSpan *)};
    }

    FORCE_INLINE
    constexpr auto set_memmap(PmFrame *map) -> void {
        DEBUG_ASSERT(is_termianl());
        m_value.set<Index::Map>(map);
    }

    FORCE_INLINE
    constexpr auto secmap() const -> SecMap {
        DEBUG_ASSERT(is_termianl() == false);
        return {static_cast<Self *>(m_value.get<Index::Map>()), PAGE_SIZE / sizeof(*this)};
    }

    FORCE_INLINE
    constexpr auto set_secmap(Self *map) -> void {
        DEBUG_ASSERT(is_termianl() == false);
        m_value.set<Index::Map>(map);
    }

    FORCE_INLINE
    constexpr auto has_map() const -> bool {
        return m_value.get<Index::Map>() == nullptr;
    }

    FORCE_INLINE
    constexpr auto is_state_present() const -> bool {
        return state() == State::Present;
    }

    FORCE_INLINE
    constexpr auto is_state_online() const -> bool {
        return state() == State::Online;
    }

    FORCE_INLINE
    constexpr auto is_termianl() const -> bool {
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
        std::tuple<Field<Index::State, State, ustl::bit_width(std::to_underlying(State::Count))>,
                   Field<Index::Type, Type, ustl::bit_width(std::to_underlying(Type::Count))>,
                   Field<Index::Level, usize, ustl::bit_width<usize>(MAX_LEVEL)>,
                   // FIXME(SmallHuaZi): Hard code.
                   Field<Index::Map, void *, 58, true>>>;
    Value m_value;
};

/// `MemMap` is a high-level abstract and implementation to physical memory layout.
/// It's responsibility is to pre-allocate `PmFrame` and to provide a group of the convertion
/// method among `PmFrame`, `Pfn`, `SecNum`, `VirtAddr` and `PhysAddr`.
/// The sparse memory model: a multi-level table of MemSection, each leaf owning
/// the descriptors for one section's worth of pfns.
///
/// A class rather than a pair of free functions because the section table is
/// real state with real invariants -- levels, presence, terminal-vs-interminal
/// -- and MemSection exists to enforce them. What a class must *not* own is the
/// memmap bring-up; see ours/mm/memmap_init.hpp for why that lives outside.
class SparseMemMap {
    using Self = SparseMemMap;

  public:
    /// The one instance.
    FORCE_INLINE
    static auto get() -> Self & {
        static Self MODEL;
        return MODEL;
    }

    constexpr static usize MAPPING_LEVEL = MemSection::MAX_LEVEL;

    constexpr static usize TOP_LEVEL = MAPPING_LEVEL - 1;

    constexpr static usize ENTRIES_PER_SECTION = PAGE_SIZE / sizeof(MemSection);

    constexpr static usize FRAMES_PER_SECTION = usize{1} << (SECTION_SHIFT - PAGE_SHIFT);

    // Each section's descriptors are one array, allocated on its own
    // (activate_section()), so two sections' arrays need be neither adjacent
    // nor in order. The buddy reaches a block's partner by offsetting the
    // descriptor in hand, which stays defined only while both are in one
    // array: so a max-order block, the furthest it ever reaches, must fit in
    // one section.
    static_assert(MAX_FRAME_ORDER + PAGE_SHIFT <= SECTION_SHIFT,
                  "a max-order block must not cross a memmap section");

    constexpr static auto LEAVES_PER_LEVEL = []<usize... Levels>(ustl::IndexSequence<Levels...>) {
        static_assert(sizeof...(Levels) == MAPPING_LEVEL);
        std::array<usize, sizeof...(Levels)> array;
        // L0 entry is a leaf itself.
        array[0] = 1;
        for (usize i = 1; i < array.size(); ++i) {
            array[i] = array[i - 1] * ENTRIES_PER_SECTION;
        }
        return array;
    }(ustl::MakeIndexSequenceT<MAPPING_LEVEL>());

    constexpr static auto FRAMES_PER_LEVEL = []<usize... Levels>(ustl::IndexSequence<Levels...>) {
        static_assert(sizeof...(Levels) == MAPPING_LEVEL);
        std::array<usize, sizeof...(Levels)> array;
        array[0] = usize{1} << (SECTION_SHIFT - PAGE_SHIFT);
        for (usize i = 1; i < array.size(); ++i) {
            array[i] = array[i - 1] * ENTRIES_PER_SECTION;
        }
        return array;
    }(ustl::MakeIndexSequenceT<MAPPING_LEVEL>());

    constexpr static usize INDEX_BITS = {
        (ustl::NumericLimits<usize>::DIGITS - SECTION_SIZE_BITS + MAPPING_LEVEL) / MAPPING_LEVEL};

    /// How far init_memmap() rounds the tail of the last range up: one section,
    /// the unit this model allocates descriptors in.
    constexpr static usize FRAMES_PER_MEMMAP_CHUNK = FRAMES_PER_SECTION;

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

    FORCE_INLINE
    constexpr auto pfn_to_section(Pfn pfn) const -> MemSection * {
        return secnum_to_section(pfn_to_secnum(pfn));
    }

    FORCE_INLINE
    constexpr auto secnum_to_section(SecNum secnum) const -> MemSection * {
        return priv_secnum_to_section(secnum);
    }

    FORCE_INLINE
    constexpr auto pfn_to_frame(Pfn pfn) -> PmFrame * {
        if (auto const section = secnum_to_section(pfn_to_secnum(pfn))) {
            return ustl::mem::address_of(section->memmap()[pfn & PFN_SECTION_MASK]);
        }

        return nullptr;
    }

    FORCE_INLINE
    constexpr auto frame_to_pfn(PmFrame const *frame) -> Pfn {
        DEBUG_ASSERT(frame != nullptr);
        if (auto const section = frame_to_section(frame)) {
            return secnum_to_pfn(frame->secnum()) + (frame - section->memmap().data());
        }

        return 0;
    }

    FORCE_INLINE
    constexpr auto frame_to_section(PmFrame const *frame) -> MemSection * {
        DEBUG_ASSERT(frame != nullptr);
        return secnum_to_section(frame->secnum());
    }

    auto add_range(PhysAddr start, PhysAddr end, NodeId nid) -> Status;

    auto remove_range(PhysAddr start, PhysAddr end, NodeId nid) -> Status;

    /// Builds the root of the section table. Everything else this model does
    /// descends from it, so it runs before any range is added.
    INIT_CODE
    auto init() -> Status;

    auto dump() -> void;

  private:
    class Impl;

    SparseMemMap() = default;
    ~SparseMemMap() = default;

    USTL_NOT_COPYABLE_AND_MOVEABLE(SparseMemMap);

    template <typename F>
    auto descend(SecNum start, SecNum end, F &&f) -> Status;

    auto init_index(usize level, SecNum begin, NodeId nid) -> ustl::Result<MemSection *, Status>;

    FORCE_INLINE
    auto activate_section(SecNum secnum, NodeId nid) -> ustl::Result<FrameSpan, Status> {
        auto section = secnum_to_section(secnum);
        if (!section) {
            return ustl::err(Status::OutOfMem);
        }

        return activate_section(section, nid);
    }

    /// Allocate memmap for the given section and mark it active.
    auto activate_section(MemSection *section, NodeId nid) -> ustl::Result<FrameSpan, Status>;

    MEM_INIT_CODE
    auto populate_memmap(Pfn start, usize nr_frames, NodeId nid) -> ustl::Result<FrameSpan, Status>;

    auto depopulate_memmap(Pfn start, usize nr_frames, NodeId nid) -> Status;

    auto mark_section(SecNum begin, SecNum end, MemSection::State) -> void;

    FORCE_INLINE
    constexpr static auto secnum_to_index(usize level, SecNum secnum) -> usize {
        constinit static auto const SHIFT = []() {
            std::array<usize, MAPPING_LEVEL> array;
            array[0] = 0;
            for (usize i = 1; i < array.size(); ++i) {
                array[i] = array[i - 1] + INDEX_BITS;
            }
            return array;
        }();

        return (secnum >> SHIFT[level]) & ((usize{1} << INDEX_BITS) - 1);
    }

    FORCE_INLINE
    constexpr auto priv_secnum_to_section(SecNum secnum) const -> MemSection * {
        DEBUG_ASSERT(m_root_section);

        /// A short loop will be unfold.
        MemSection *section = m_root_section;
        for (isize level = MAPPING_LEVEL - 1; level >= 0; --level) {
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

} // namespace ours::mm::priv

#endif // #ifndef OURS_MM_PRIV_SPARSE_MEMMAP_HPP
