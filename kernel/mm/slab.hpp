/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_SLAB_HPP 
#define OURS_MM_SLAB_HPP 1

#include <ours/mm/pm-frame.hpp>
#include <ours/mm/gaf.hpp>

namespace ours::mm {
    /// When a frame is being used in `MemCache`, it's descriptor shows the following layout.
    class Slab: public PageFrame {
        using Self = Slab;
        using Base = PageFrame;
      public:
        class Object: public ustl::collections::intrusive::SlistBaseHook<
                             ustl::collections::intrusive::LinkMode<
                             ustl::collections::intrusive::LinkModeType::AutoUnlink>> {
            using Self = Object;
          public:
            USTL_DECLARE_SLIST(Object, List, ustl::collections::intrusive::ConstantTimeSize<false>);
        };
        static_assert(sizeof(Object) == sizeof(usize));
        static_assert(sizeof(Object::List) == sizeof(usize));

        static constexpr auto ROLE = Role::Slab;

        static auto create(MemCache *owner, Gaf gaf, usize order, usize size, NnId nid) -> Self *;

        auto allocate(Gaf gaf) -> void *;
      private:
        friend class MemCache;

        auto init(MemCache *owner, usize size) -> Status;

        auto destroy() -> void;

        auto get_object() -> void *;

        auto put_object(void *object) -> void;

        auto has_object() const -> bool;

        Object::List m_object;
        u16 m_num_inuse;
        u16 m_num_objects;

        MemCache *m_owner;

        using AutoUnlinkMode = ustl::collections::intrusive::LinkMode<
            ustl::collections::intrusive::LinkModeType::AutoUnlink>;

        ustl::collections::intrusive::ListMemberHook<AutoUnlinkMode> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, ManagedOption);
      public:
        USTL_DECLARE_LIST(Slab, List, ManagedOption, ustl::collections::intrusive::ConstantTimeSize<false>);
    };
} // namespace ours::mm

#endif // #ifndef OURS_MM_SLAB_HPP