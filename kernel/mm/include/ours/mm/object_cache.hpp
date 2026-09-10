/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_OBJECT_CACHE_HPP
#define OURS_MM_OBJECT_CACHE_HPP 1

#include <ours/mm/cfg.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/pm_frame.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sync/mutex.hpp>

#include <array>

#include <gktl/canary.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <ustl/collections/intrusive/list.hpp>
#include <ustl/mem/object.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
class MemCache: public ustl::RefCounted<MemCache, ustl::sync::ThreadSafePolicy> {
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

        /// Back this cache's slabs with folios (compound pages).
        ///
        /// TODO(SmallHuaZi): accepted and stored, but nothing acts on it yet --
        /// Slab::create() still allocates the same way regardless. The flag
        /// exists because every ObjectCache call site asks for it by name.
        Folio = BIT(13),
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Option);

    static auto init() -> Status;

    static auto create(char const *name, usize size, usize align, Option, ustl::Arc<Self> *)
        -> Status;

    static auto create_boot(Self &self, char const *name, usize size, usize align, Option)
        -> Status;

    auto allocate(Gaf gaf, NnId nid = NUMA_MAX_NODE) -> void *;

    auto free(void *) -> void;

    auto alias(char const *name, usize size, usize align, Option, ustl::Arc<Self> *) -> Status;

    FORCE_INLINE
    auto name() const -> char const * {
        return m_name;
    }

  private:
    struct CpuCache;
    struct NodeCache;

    friend class ustl::RefCounted<MemCache, ustl::sync::ThreadSafePolicy>;

    MemCache(char const *name, usize size, usize align, Option);
    ~MemCache();

    /// Storage reclaim for MemCache instances: return the storage to the
    /// self-cache (`SELF_CACHE->free`). Set by the constructor.
    static auto reclaim_storage(void *pointer) noexcept -> void;

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
    std::array<NodeCache *, NUMA_MAX_NODE> m_node_caches;

    ustl::collections::intrusive::ListMemberHook<> m_managed_hook;
    USTL_DECLARE_LIST_MEMBER(Self, List, m_managed_hook);

    static sync::Mutex CACHE_LOCK;
    static List GLOBAL_CACHES TA_GUARDED(CACHE_LOCK);

    /// Not guarded by CACHE_LOCK, deliberately. init() fills it in, once,
    /// before the first cache exists, and nothing writes it afterwards; the
    /// allocation path reads it on every slab refill. Putting a sleeping
    /// Mutex on that read would be wrong twice over -- the hot path cannot
    /// take it, and a lock protects against writers, of which there are none
    /// after bring-up. Linux's slab_nodes is the same shape: written by
    /// slab_node_online()/offline() under slab_mutex on the hotplug path,
    /// read lock-free by the allocator.
    static NodeMask SLAB_NODES;
};

/// The typed shell over MemCache.
///
/// MemCache is typeless: it hands out `size`-byte chunks as `void *` and knows
/// nothing about what goes in them. That is the right shape for the general
/// allocation paths -- ktl's `operator new` and kmalloc's size-class caches
/// use it directly.
///
/// ObjectCache is what you reach for at the other end: when one *particular*
/// type is allocated and freed often enough to deserve a cache of its own
/// (VmArea, VmMapping, Thread, ...). It adds exactly the type knowledge
/// MemCache lacks -- sizeof/alignof when creating the cache, and construction
/// and destruction around each allocation.
///
/// ObjectCache itself is only a scope: `Instance<T, Options...>` is the cache.
class ObjectCache {
  public:
    /// Cache creation flags. These are MemCache's own options; ObjectCache
    /// adds no policy of its own.
    using Flags = MemCache::Option;

    struct Option;

    template <typename T, typename... Options>
    class Instance;

    ObjectCache() = delete;
};

/// Options accepted by ObjectCache::Instance.
struct ObjectCache::Option {
    /// Records the name, flags and init level a cache should be created with,
    /// so the declaration of the cache states them in one place.
    template <ustl::util::FixedString Name, ObjectCache::Flags Flags, gktl::InitLevel InitLevel>
    struct AddInitHook;
};

template <ustl::util::FixedString Name, ObjectCache::Flags Flags, gktl::InitLevel InitLevel>
struct ObjectCache::Option::AddInitHook {
    /// Heap is where the object caches themselves come up (mm::init_cache()),
    /// so it is the earliest level at which MemCache::create() can succeed.
    ///
    /// This used to read `InitLevel > gktl::InitLevel::Platform`, which is the
    /// *last* level of all -- it would have rejected every usable value. It
    /// never fired because nothing instantiated this template: the option was
    /// only ever named as a template argument, and its members went unused
    /// while the hooks that should have read them were missing.
    static_assert(InitLevel >= gktl::InitLevel::Heap,
                  "An object cache cannot be created before the heap exists");

    constexpr static auto NAME = Name;
    constexpr static auto FLAGS = Flags;
    constexpr static auto INIT_LEVEL = InitLevel;
};

/// One cache serving every allocation of T.
template <typename T, typename... Options>
class ObjectCache::Instance {
    using Self = Instance;

  public:
    static auto init(char const *name, Flags flags) -> Status {
        return MemCache::create(name, sizeof(T), alignof(T), flags, &CACHE);
    }

    /// Initializes from an AddInitHook option's recorded name and flags.
    ///
    /// The option only carries those values; it deliberately does not emit the
    /// GKTL_INIT_HOOK itself. A hook declared inside a class template is a
    /// static data member, and those are instantiated only when odr-used, so
    /// it would silently never be emitted. Callers therefore place a plain
    /// namespace-scope GKTL_INIT_HOOK that calls this.
    static auto init_from_option() -> Status
        requires(sizeof...(Options) == 1)
    {
        return init_one<Options...>();
    }

    [[nodiscard]]
    static auto is_ready() -> bool {
        return bool(CACHE);
    }

    /// Hands back uninitialized storage for one T, for the two-phase callers
    /// that construct it themselves later (Thread::spawn_etc placement-news
    /// into what Thread::spawn allocated here). Prefer get() otherwise.
    static auto allocate(Gaf gaf, NnId nid = NUMA_MAX_NODE) -> T * {
        DEBUG_ASSERT(CACHE, "ObjectCache for this type is not initialized");
        return static_cast<T *>(CACHE->allocate(gaf, nid));
    }

    /// Releases storage from allocate() without running the destructor.
    static auto deallocate(T *storage) -> void {
        if (storage) {
            CACHE->free(storage);
        }
    }

    template <typename... Args>
    static auto get(Gaf gaf, Args &&...args) -> T * {
        DEBUG_ASSERT(CACHE, "ObjectCache for this type is not initialized");

        auto *const storage = CACHE->allocate(gaf, NUMA_MAX_NODE);
        if (!storage) {
            return nullptr;
        }
        return new (storage) T(ustl::forward<Args>(args)...);
    }

    template <typename... Args>
    static auto get(Gaf gaf, NnId nid, Args &&...args) -> T * {
        DEBUG_ASSERT(CACHE, "ObjectCache for this type is not initialized");

        auto *const storage = CACHE->allocate(gaf, nid);
        if (!storage) {
            return nullptr;
        }
        return new (storage) T(ustl::forward<Args>(args)...);
    }

    static auto put(T *object) -> void {
        if (object) {
            object->~T();
            CACHE->free(object);
        }
    }

  private:
    template <typename Option>
    static auto init_one() -> Status {
        // `.value`, not `.data()`: ustl::util::FixedString is an aggregate over
        // a public `char value[N]` and has no data(). This never failed to
        // compile before because nothing instantiated this path -- the hooks
        // that would have called it were missing.
        return init(Option::NAME.value, Option::FLAGS);
    }

    static inline ustl::Arc<MemCache> CACHE;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_OBJECT_CACHE_HPP
