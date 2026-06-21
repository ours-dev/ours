/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_OBJECT_CACHE_HPP
#define OURS_MEM_OBJECT_CACHE_HPP 1

#include <ours/mem/gaf.hpp>
#include <ours/mem/cfg.hpp>
#include <ours/mem/pm-frame.hpp>
#include <ours/mem/pmm.hpp>
#include <ours/mem/cpu-local.hpp>

#include <ours/sync/mutex.hpp>

#include <ustl/rc.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/collections/intrusive/list.hpp>

#include <logz4/log.hpp>

#include <gktl/canary.hpp>
#include <gktl/init_hook.hpp>

namespace ours::mem {
    class Slab;

    class MemCache: public ustl::RefCounter<MemCache> {
        using Self = MemCache;
      public:
        enum class Option {
            Dma = BIT(0),
            Dma32 = BIT(1),

            /// Requires to align objects on cache line boundaries.
            CacheAlign = BIT(4),

            /// DEBUG
            RedZone = BIT(6),
            /// DEBUG
            Poison = BIT(7),

            /// Capablity to permit that |this| merged with another.
            Merge = BIT(10),
            /// Capablity to permit that |this| is reclaimed by Deamon.
            Reclaim = BIT(11),
            /// Capablity to permit. 
            Alias = BIT(12),
        };
        USTL_ENABLE_INNER_ENUM_BITMASK(Option);

        static auto init() -> Status;

        static auto create(char const *name, usize size, usize align, Option, ustl::Rc<Self> *) -> Status;

        static auto create_boot(Self &self, char const *name, usize size, usize align, Option) -> Status;

        auto allocate(Gaf gaf, NnId nid = NUMA_MAX_NODE) -> void *;

        auto free(void *) -> void;

        auto alias(char const *name, usize size, usize align, Option, ustl::Rc<Self> *) -> Status;

        FORCE_INLINE
        auto name() const -> char const * {
            return m_name;
        }
      private:
        struct CpuCache;
        struct NodeCache;

        MemCache(char const *name, usize size, usize align, Option);
        ~MemCache();

        auto init(char const *name, usize size, usize align, Option) -> Status;

        auto compute_layout(usize size, Option flags) -> Status;

        auto allocate(Gaf gaf, NnId nid, CpuCache *cc) -> void *;

        gktl::Canary m_canary;
	    char const *m_name;
        Option m_option;

        /// The size of bytes ocuppied by the object including metadata.
        u32 m_phys_size;
        /// The size of bytes of user object. 
        u32 m_user_size;
        /// The offset of pointer to next object in the object
        u32 m_next_offset;
        /// The alignment of user object. 
        u32 m_user_align;
        /// The size of bytes ocuppied by the redzone and user object.
        u32 m_inuse;

        u32 m_red_left_pad;

        u32 m_min_partial;

        u16 m_num_objects;
        u16 m_order;

        u16 m_min_num_objects;
        u16 m_min_order;

	      Gaf m_gaf;

        CpuLocal::PerCpu<CpuCache> m_cpu_caches;
        ustl::Array<NodeCache *, NUMA_MAX_NODE> m_node_caches;

        ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
        USTL_DECLARE_LIST_MEMBER(Self, List, m_managed_hook);

        static sync::Mutex g_cache_lock;
        static List g_global_caches  TA_GUARD(g_cache_lock);
        static NodeMask g_slab_nodes TA_GUARD(g_cache_lock);
    };

    template <typename T, usize Size, usize Alignment>
    class ObjectCache {
        using Self = ObjectCache;
      public:
        static auto init(char const *name, MemCache::Option option) -> Status;

        template <typename... Args>
        static auto get(Gaf gaf, Args&&... args) -> T * {
            DEBUG_ASSERT(g_cache_instance, "ObjectCache[{}] is not initialized", g_cache_instance->name());

            auto self = g_cache_instance->allocate(gaf, NUMA_MAX_NODE);
            if (!self) {
                return self;
            }
            return new (self) T(ustl::forward<Args>(args)...);
        }

        template <typename... Args>
        static auto get(Gaf gaf, NnId nid, Args&&... args) -> T * {
            DEBUG_ASSERT(g_cache_instance, "ObjectCache[{}] is not initialized", g_cache_instance->name());

            auto self = g_cache_instance->allocate(gaf, nid);
            if (!self) {
                return self;
            }
            return new (self) T(ustl::forward<Args>(args)...);
        }

        static auto put(T *) -> void;
      private:
        static inline ustl::Rc<MemCache> g_cache_instance;
    };

    template <typename... Options>
    class BasicObjectCacheBuilder {
        template <ustl::util::FixedString Name, MemCache::Option Option, gktl::InitLevel InitLevel>
        class DoAddInitHook;
      public:
        /// Add a hook function to initialize the object-cache.
        template <ustl::util::FixedString Name, MemCache::Option Option, gktl::InitLevel InitLevel>
        using AddInitHook = BasicObjectCacheBuilder<Options..., DoAddInitHook<Name, Option, InitLevel>>;

        template <typename T>
        using Build = ObjectCache<T, Options...>;
    };

    template <typename... Options>
    template <ustl::util::FixedString CacheName, MemCache::Option Option, gktl::InitLevel InitLevel>
    class BasicObjectCacheBuilder<Options...>::DoAddInitHook {
        static_assert(InitLevel > gktl::InitLevel::Platform, "Requires at least the platform initialized.");

        INIT_CODE
        static auto init_hook() -> void {
            auto const status = MemCache::init(CacheName.data(), Option);
            if (Status::Ok != status) {
                panic("Failed to create object cache \"{}\"", CacheName.data());
            }
            log::trace("\"{}\" has been created", CacheName.data());
        }

        GKTL_INIT_HOOK(InitHook, init_hook , InitLevel);
    };

    using ObjectCacheBuilder = BasicObjectCacheBuilder<void>;
} // namespace ours::mem

#endif // #ifndef OURS_MEM_OBJECT_CACHE_HPP