/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_RESOURCE_DISPATCHER_HPP
#define OURS_OBJECT_RESOURCE_DISPATCHER_HPP 1

#include <ours/object/types.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/syscall/resource.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace ours::object {
    using ResourceDispatcherBase = SoloDispatcher<ResourceDispatcher, kDefaultRights>;

    class ResourceDispatcher: public ResourceDispatcherBase,
                              public ustl::collections::intrusive::ListBaseHook<> {
        using Base = ResourceDispatcherBase;
        using Self = ResourceDispatcher;
      public:
        static constexpr auto kMaxNameSize = 32;

        using Kind = RsrcKind;
        using Flag = RsrcFlag;
        using Name = ktl::Name<kMaxNameSize>;
        using List = ustl::collections::intrusive::List<Self>;

        struct Storage {
            Self::List resources;
        };

        static auto create(KernelHandle<Self> *out, Rights &rights, Kind kind, 
                           PhysAddr base, PhysAddr size, Flag flags, 
                           Name const &name, Storage *storage = nullptr) -> Status;
      private:
        ResourceDispatcher(Kind kind, PhysAddr base, PhysAddr size, Flag flags, Name const &);
        ~ResourceDispatcher() = default;

        static constexpr auto MAGIC = gktl::magic("ResourceDispatcher");

        gktl::Canary m_canary;
        Name m_name;
        Kind m_kind;
        Flag m_flags;
        PhysAddr m_base;
        PhysAddr m_size;
        Self::List *m_rsrclist;

        static inline Storage g_system_resources;
    };
} // namespace ours::object

#endif // OURS_OBJECT_RESOURCE_DISPATCHER_HPP
