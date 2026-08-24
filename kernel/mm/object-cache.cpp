#include "slab.hpp"
#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <ktl/new.hpp>
#include <logz4/log.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/early-mem.hpp>
#include <ours/mm/object-cache.hpp>
#include <ours/mm/pm-node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sync/local-lock.hpp>
#include <ours/task/auto-preempt-disabler.hpp>
#include <ustl/lazy_init.hpp>

#define CONFIG_MEM_CACHE_DEBUG 1
#define CONFIG_MEM_CACHE_CPU_PARTIAL 1

namespace ours::mm {
namespace {
    GKTL_COUNTER(object_cache_free_slow_path, "mem.object_cache.free.slow_path");
    GKTL_COUNTER(object_cache_free_fast_path, "mem.object_cache.free.fast_path");
    GKTL_COUNTER(object_cache_allocate_slow_path, "mem.object_cache.allocate.slow_path");
    GKTL_COUNTER(object_cache_allocate_fast_path, "mem.object_cache.allocate.fast_path");

    MemCache *g_self_cache;

    MemCache *g_node_cache;

    INIT_DATA
    ustl::LazyInit<MemCache> g_bootstrap_self_cache;

    INIT_DATA
    ustl::LazyInit<MemCache> g_bootstrap_node_cache;

    u32 g_slab_min_order;
    u32 g_slab_max_order;
    u32 g_slab_min_objects;

    FORCE_INLINE
    auto calculate_order_objects(u32 order, u32 size) -> u32 {
        return (order << PAGE_SHIFT) / size;
    }

    auto calculate_order(u32 size) -> u32 {
        auto min_objects = g_slab_min_objects;
        if (!min_objects) {
            auto const nr_cpus = Cpu::num_present_cpus();
            min_objects = sizeof(usize) * ustl::bit_ceil(nr_cpus);
        }

        using namespace ustl::algorithms;
        auto max_objects = max<u32>(calculate_order_objects(g_slab_max_order, size), 1);
        min_objects = min(min_objects, max_objects);

        auto min_order = max<u32>(g_slab_min_order, get_order(min_objects * size));
    }
} // namespace

auto Slab::destroy() -> void {
    DEBUG_ASSERT(m_num_inuse == 0);

    this->put();
    m_hook.unlink();
    mm::free_frames(as_ptr<PmFrame>(), order());
}

auto Slab::get_object() -> void * {}

auto Slab::put_object(void *object) -> void {}

auto Slab::init(MemCache *owner, usize size) -> Status {
    m_owner = owner;
    return Status::Ok;
}

auto Slab::create(MemCache *owner, Gaf gaf, usize order, usize size, NnId nid) -> Slab * {
    auto frame = mm::alloc_frames(nid, gaf, order);
    if (!frame) {
        return nullptr;
    }

    auto slab = frame->as_ptr<Self>();
    if (auto status = slab->init(owner, size); Status::Ok != status) {
        mm::free_frames(frame, order);
        return nullptr;
    }

    slab->get();
    return slab;
}

struct MemCache::CpuCache {
    static auto on_mem_cache_init(MemCache &cache) -> Status;
    static auto on_mem_cache_free(MemCache &cache) -> void;

    auto acquire_slab(NnId nid) -> Slab *;

    auto deactivate_slab(Slab *) -> void;

    Slab::Object::List::IterMut m_objects;
    usize transaction_id;

    Slab *active;
    Slab::List partials;

    [[no_unique_address]]
    sync::LocalLock lock;
};

auto MemCache::CpuCache::on_mem_cache_init(MemCache &cache) -> Status {
    auto cpu_cache = CpuLocal::allocate<CpuCache>(1, alignof(CpuCache));
    if (!cpu_cache) {
        return Status::NoMem;
    }

    cpu_cache.for_each([](CpuNum cpu, CpuCache &self) { self.transaction_id = cpu; });

    cache.m_cpu_caches = ustl::move(cpu_cache);
    return Status::Ok;
}

auto MemCache::CpuCache::acquire_slab(NnId nid) -> Slab * {
    auto slab = ktl::read_once(active);
    if (slab != nullptr) {
        if (slab->nid() == nid) {
            // #1 Best case, the slab is already in the correct node.
            return slab;
        }

        // #2 The slab is not in the node we specified, to deactivate it
        // and to put it into the paritial list.
        deactivate_slab(slab);
    } else if (nid != NUMA_NO_NODE && !g_slab_nodes.test(nid)) [[unlikely]] {
        nid = NUMA_NO_NODE;
    }

    if constexpr (CONFIG_MEM_CACHE_CPU_PARTIAL) {
        Slab::List to_remove;

        auto iter = partials.begin(), end = partials.end();
        for (; iter != end; ++iter) {
            Guard<sync::LocalLock, IrqSave> guard{lock};

            if (slab->nid() == nid) {
                slab = &*iter;
                partials.erase(iter);
                break;
            }
        }
        partials.splice(iter, to_remove, iter, end);
    }

    return slab;
}

auto MemCache::CpuCache::deactivate_slab(Slab *) -> void {}

struct MemCache::NodeCache {
    static auto on_mem_cache_init(MemCache &cache) -> Status;
    static auto on_mem_cache_free(MemCache &cache) -> void;

    sync::SpinLock lock;
    Slab::List partials;
    usize num_partials;
};

auto MemCache::NodeCache::on_mem_cache_init(MemCache &cache) -> Status {
    auto const &node = g_slab_nodes;
    for (auto nid = 0; nid < node.size(); ++nid) {
        if (!node.test(nid)) {
            continue;
        }

        auto node_cache = g_node_cache->allocate(cache.m_gaf, nid);
        if (!node_cache) {
            // Release all holding and report error.
            on_mem_cache_free(cache);
            return Status::NoMem;
        }

        cache.m_node_caches[nid] = new (node_cache) NodeCache();
    }

    return Status::Ok;
}

auto MemCache::NodeCache::on_mem_cache_free(MemCache &cache) -> void {
    for (auto node_cache: cache.m_node_caches) {
        g_node_cache->free(node_cache);
    }
}

constexpr static auto const kMaxPartialSlabs = 8U;

constexpr static auto const kMinPartialSlabs = 4U;

constexpr static auto const kRedInactive = 0xccU;

constexpr static auto const kRedActive = 0xddU;

constexpr static auto const kPoisonInuse = 0x5aU;

constexpr static auto const kPoisonFree = 0x6bU;

constexpr static auto const kPoisonEnd = 0xa5U;

MemCache::MemCache(char const *name, usize size, usize align, Option option)
    : m_name(name)
    , m_phys_size(ustl::mm::align_up(size, sizeof(usize)))
    , m_user_size(align)
    , m_order(0)
    , m_num_objects(0)
    , m_option(option)
    , m_gaf(Gaf::Compound)
    , m_min_partial(kMinPartialSlabs) {
    // Every MemCache instance's storage comes from the self-cache, so the
    // refcount machinery must reclaim it there instead of `delete`.
    set_deallocate(&Self::reclaim_storage);
}

auto MemCache::reclaim_storage(void *pointer) noexcept -> void {
    g_self_cache->free(pointer);
}

MemCache::~MemCache() noexcept {
    CpuCache::on_mem_cache_free(*this);
    NodeCache::on_mem_cache_free(*this);
}

auto MemCache::init() -> Status {
    g_self_cache = g_bootstrap_self_cache.data();
    g_node_cache = g_bootstrap_node_cache.data();

    // Initialize the nodemask to nodes which has normal memories.
    PmNode::for_each_nid(PmNode::State::Memory, [](NnId nid) { g_slab_nodes.set(nid); });

    Self::create_boot(*g_node_cache, "cache:node", sizeof(NodeCache), 0, Option::CacheAlign);
    Self::create_boot(*g_self_cache, "cache:self", sizeof(MemCache), 0, Option::CacheAlign);
}

auto MemCache::compute_layout(usize size, Option option) -> Status {
    constexpr auto kWordSize = sizeof(void *);

    // At least keep a size of FP.
    usize phys_size = ustl::mm::align_up(size, kWordSize);

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        // Check if there is left spaces for readzone between the end of object and
        // word boundaries. May it be the readzone rather than additional spaces.
        if (!!(option & Option::RedZone) && phys_size == m_user_size) {
            phys_size += kWordSize + size;
        }
    }

    m_inuse = phys_size;

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        // Under debug mode and debug options enabled, only placing the FP after
        // user object is permitted.
        if (!!(option & Option::Poison) ||
            (!!(option & Option::RedZone) && m_user_size < phys_size)) {
            m_next_offset = phys_size;
            phys_size += kWordSize; // Put the FP
        } else {
            m_next_offset = ustl::mm::align_down(m_user_size / 2, kWordSize);
        }
    } else {
        // Default case, we store freelist pointer near middle of object.
        m_next_offset = ustl::mm::align_down(m_user_size / 2, kWordSize);
    }

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        if (!!(option & Option::RedZone)) {
            m_red_left_pad = ustl::mm::align_up(kWordSize, m_user_align);
            phys_size += m_red_left_pad;
        }
    }

    phys_size = ustl::mm::align_up(phys_size, m_user_align);

    auto const order = calculate_order(phys_size);
    if (static_cast<i32>(order) < 0) {
        return Status::InternalError;
    }

    m_phys_size = phys_size;
    m_order = order;
    m_num_objects = (order << PAGE_SHIFT) / phys_size;
    m_min_order = mm::get_order(phys_size);
    m_min_num_objects = (m_min_order << PAGE_SHIFT) / phys_size;
    return Status::Ok;
}

auto MemCache::init(char const *name, usize size, usize align, Option option) -> Status {
    auto status = compute_layout(size, option);
    if (Status::Ok != status) {
        return status;
    }

    if (!!(option & Option::Dma)) {
        m_gaf |= Gaf::ZoneDma;
    }
    if (!!(option & Option::Dma32)) {
        m_gaf |= Gaf::ZoneDma32;
    }
    if (!!(option & Option::Reclaim)) {
        m_gaf |= Gaf::Reclaim;
    }

    using namespace ustl::algorithms;
    m_min_partial = clamp(ustl::bit_ceil(m_phys_size) / 2, kMinPartialSlabs, kMaxPartialSlabs);

    status = NodeCache::on_mem_cache_init(*this);
    if (Status::Ok != status) {
        return status;
    }

    status = CpuCache::on_mem_cache_init(*this);
    if (Status::Ok != status) {
        return status;
    }

    {
        Guard guard{g_cache_lock};
        g_global_caches.push_back(*this);
    }
    return Status::Ok;
}

auto MemCache::alias(const char *name, usize size, usize align, Option, ustl::Arc<Self> *)
    -> Status {}

auto MemCache::create(char const *name, usize size, usize align, Option option,
                      ustl::Arc<Self> *out) -> Status {
    Guard guard{g_cache_lock};

    auto raw_self = g_self_cache->allocate(kGafKernel);
    if (!raw_self) {
        return Status::NoMem;
    }

    auto cleanup = gktl::defer([raw_self]() { g_self_cache->free(raw_self); });

    auto self = new (raw_self) Self(name, size, align, option);

    auto status = self->init(name, size, align, option);
    if (Status::Ok != status) {
        return status;
    }

    *out = ustl::Arc<Self>::adopt(self);
    cleanup.cancel();
    return Status::Ok;
}

auto MemCache::create_boot(Self &self, char const *name, usize size, usize align, Option options)
    -> Status {
    Guard guard{g_cache_lock};
}

auto MemCache::allocate(Gaf gaf, NnId nid, CpuCache *cache) -> void * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE);

    if (!g_slab_nodes.test(nid)) {
        nid = NUMA_NO_NODE;
    }

    auto slab = ktl::read_once(cache->active);
    if (slab && (nid == NUMA_NO_NODE || slab->nid() == nid)) {
        // Deactivate slab
    }

    if (!slab) {
    }

    if (!slab || (slab && (nid == NUMA_NO_NODE || slab->nid() == nid))) {
        if constexpr (CONFIG_MEM_CACHE_CPU_PARTIAL) {
            auto node_cache = m_node_caches[nid];
            Slab::List to_remove;

            auto iter = cache->partials.begin(), end = cache->partials.end();
            for (; iter != end; ++iter) {
                Guard<sync::LocalLock, IrqSave> guard{cache->lock};

                if (slab->nid() == nid) {
                    slab = &*iter;
                    cache->partials.erase(iter);
                    break;
                }
            }
            cache->partials.splice(iter, to_remove, iter, end);
        }
    }

    DEBUG_ASSERT(slab, "Here we should have a available slab");

    Guard<sync::LocalLock, IrqSave> guard{cache->lock};
}

auto MemCache::allocate(Gaf gaf, NnId nid) -> void * {
    task::AutoPreemptDisabler preempt_disabler{};
    return allocate(gaf, nid, m_cpu_caches.ptr_mut());
}
} // namespace ours::mm