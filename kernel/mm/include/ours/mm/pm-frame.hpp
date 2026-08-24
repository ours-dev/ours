/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PM_FRAME_HPP
#define OURS_MM_PM_FRAME_HPP 1

#include <ours/assert.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/types.hpp>
#include <ustl/bit.hpp>
#include <ustl/bitfields.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/option.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/traits/is_base_of.hpp>
#include <ustl/traits/is_same.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::mm {
namespace page {
    enum class Role {
        None,
        Io,
        Lru,
        Vmm,
        Pmm,
        Slab,
        Heap,
        Folio,
        Count,
    };

    enum class Flags {
        Active = BIT(0),
        Pinned = BIT(1),
        Dirty = BIT(2),
        UpToDate = BIT(3),
        Foreign = BIT(4),
        BeginWaiting = BIT(5),
        Reclaimable = BIT(7),
        Reserved = BIT(8),

        Initialized = BIT(9),
        Count,
    };
    USTL_ENABLE_ENUM_BITMASK(Flags);

    enum class Index {
        Role,
        Flags,
        Order,
        Node,
        Section,
        ZoneType,
    };

    template <Index Index, typename Type, usize Bits, bool Enabled = true>
    using Field =
        ustl::bitfields::Field<ustl::bitfields::Id<Index>, ustl::bitfields::Type<Type>,
                               ustl::bitfields::Bits<Bits>, ustl::bitfields::Enable<Enabled>>;

    using Status = ustl::BitFields<
        Field<Index::Role, Role, ustl::bit_width(static_cast<usize>(Role::Count))>,
        Field<Index::Flags, Flags, ustl::bit_width(static_cast<usize>(Flags::Count))>,
        Field<Index::Order, usize, ustl::bit_width(static_cast<usize>(MAX_FRAME_ORDER))>,
        // Readonly after initialized
        Field<Index::ZoneType, ZoneType, ustl::bit_width(static_cast<usize>(ZoneType::Count))>,
        // Readonly after initialized
        Field<Index::Section, SecNum, MAX_PHYSADDR_BITS - SECTION_SIZE_BITS>,
        // Readonly after initialized
        Field<Index::Node, NnId, NUMA_MAX_NODES_BITS, OURS_CONFIG_NUMA>>;
} // namespace page
class PageFrame {
    using Self = PageFrame;

  public:
    using Role = page::Role;
    using Flags = page::Flags;

    /// Default-constructible so PmFrame arrays (kernel memmap and host
    /// tests) can be allocated before init() fills each descriptor.
    PageFrame() = default;

    constexpr static auto const kSizeLimit = sizeof(usize) * 8;

    constexpr static auto const kAlignment = sizeof(usize) * 8;

    auto init(ZoneType ztype, SecNum secnum, NnId nid) -> void {
        DEBUG_ASSERT(!(flags() & Flags::Initialized), "Duplicated to initialize a frame");

        m_status.set<Index::ZoneType>(ztype);
        m_status.set<Index::Section>(secnum);
        m_status.set<Index::Node>(nid);
        m_status.set<Index::Role>(Role::None);
        set_flags(Flags::Active | Flags::UpToDate | Flags::Initialized);
    }

    FORCE_INLINE constexpr auto nid() const -> NnId {
        return m_status.get<Index::Node>();
    }

    FORCE_INLINE constexpr auto zone() const -> ZoneType {
        return m_status.get<Index::ZoneType>();
    }

    FORCE_INLINE constexpr auto secnum() const -> SecNum {
        return m_status.get<Index::Section>();
    }

    FORCE_INLINE constexpr auto order() const -> usize {
        return m_status.get<Index::Order>();
    }

    FORCE_INLINE constexpr auto set_order(usize order) -> void {
        m_status.set<Index::Order>(order);
    }

    FORCE_INLINE constexpr auto mark_pinned() -> void {
        set_flags(Flags::Pinned);
    }

    FORCE_INLINE constexpr auto mark_reserved() -> void {
        set_flags(Flags::Reserved);
    }

    FORCE_INLINE constexpr auto mark_active() -> void {
        set_flags(Flags::Active);
    }

    FORCE_INLINE constexpr auto role() const -> Role {
        return m_status.get<Index::Role>();
    }

    FORCE_INLINE constexpr auto set_role(Role role) -> void {
        m_status.set<Index::Role>(role);
    }

    FORCE_INLINE constexpr auto is_role(Role role) const -> bool {
        return this->role() == role;
    }

    FORCE_INLINE constexpr auto phys_size() const -> usize {
        return BIT(order()) << PAGE_SHIFT;
    }

    FORCE_INLINE constexpr auto flags() const -> Flags {
        return m_status.get<Index::Flags>();
    }

    auto dump() const -> void;

    auto get() const -> void {
        m_references.fetch_add(1, ustl::sync::MemoryOrder::Relaxed);
    }
    auto put() const -> void {
        m_references.fetch_sub(1, ustl::sync::MemoryOrder::Relaxed);
    }

    template <typename T>
    FORCE_INLINE auto as_ref() -> T & {
        return *as_ptr<T>();
    }

    template <typename T>
    FORCE_INLINE auto as_ref() const -> T const & {
        return *as_ptr<T>();
    }

    template <typename T>
    FORCE_INLINE auto as_ptr() -> T * {
        static_assert(ustl::traits::IsBaseOfV<PageFrame, T>);
        static_assert(kSizeLimit >= sizeof(T) || ustl::traits::IsSameV<T, PmFolio>);

        m_status.set<Index::Role>(T::kRole);
        return static_cast<T *>(static_cast<PageFrame *>(this));
    }

    template <typename T>
    FORCE_INLINE auto as_ptr() const -> T const * {
        static_assert(ustl::traits::IsBaseOfV<PageFrame, T>);
        static_assert(kSizeLimit >= sizeof(T) || ustl::traits::IsSameV<T, PmFolio>);

        m_status.set<Index::Role>(T::kRole);
        return static_cast<T const *>(static_cast<PageFrame const *>(this));
    }

  protected:
    using Index = page::Index;

    USTL_NO_MOVEABLE_AND_COPYABLE(PageFrame);

    FORCE_INLINE constexpr auto set_flags(Flags flags) -> void {
        m_status.set<Index::Flags>(flags);
    }

  private:
    mutable page::Status m_status;

    // The size of following fields type should be half of a word.
    mutable ustl::sync::AtomicU32 m_references;

    // Trace the position of current frame in a large frame.
    mutable ustl::sync::AtomicU32 m_compound;
};

FORCE_INLINE inline auto operator==(PageFrame const &x, PageFrame const &y) -> bool {
    return &x == &y;
}

FORCE_INLINE inline auto operator!=(PageFrame const &x, PageFrame const &y) -> bool {
    return &x != &y;
}

/// This is a standard layout frame mainly used by PMM.
class alignas(PageFrame::kAlignment) PmFrame: public PageFrame {
    using Self = PmFrame;
    using Base = PageFrame;

  public:
    constexpr static auto kRole = Role::Pmm;

  private:
    ustl::collections::intrusive::ListMemberHook<> m_hook;

  public:
    USTL_DECLARE_LIST_MEMBER(Self, List, m_hook,
                             ustl::collections::intrusive::ConstantTimeSize<false>);
};

struct PmFolio: public PmFrame {
    constexpr static auto kRole = Role::Folio;

    PageFrame second_metadata;
};

auto to_string(PageFrame::Role role) -> char const *;

} // namespace ours::mm

#endif // #ifndef OURS_MM_PM_FRAME_HPP
