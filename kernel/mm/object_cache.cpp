#include <ours/lockdep.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/pm_node.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/sync/local_lock.hpp>
#include <ours/task/auto_preempt_disabler.hpp>

#include "slab.hpp"
#include <gktl/counter.hpp>
#include <gktl/defer.hpp>
#include <heap/new.hpp>
#include <logz4/log.hpp>
#include <ustl/lazy_init.hpp>

#define CONFIG_MEM_CACHE_DEBUG 1
// Currently inert: the per-cpu partial list was removed because it cannot be
// made race-free against the node list without a per-slab lock. See the note
// in MemCache::CpuCache for what bringing it back would take.
#define CONFIG_MEM_CACHE_CPU_PARTIAL 1

namespace ours::mm {
namespace {
    GKTL_COUNTER(OBJECT_CACHE_FREE_SLOW_PATH, "mem.object_cache.free.slow_path");
    GKTL_COUNTER(OBJECT_CACHE_FREE_FAST_PATH, "mem.object_cache.free.fast_path");
    GKTL_COUNTER(OBJECT_CACHE_ALLOCATE_SLOW_PATH, "mem.object_cache.allocate.slow_path");
    GKTL_COUNTER(OBJECT_CACHE_ALLOCATE_FAST_PATH, "mem.object_cache.allocate.fast_path");

    MemCache *SELF_CACHE;

    MemCache *NODE_CACHE;

    INIT_DATA
    ustl::LazyInit<MemCache> BOOTSTRAP_SELF_CACHE;

    INIT_DATA
    ustl::LazyInit<MemCache> BOOTSTRAP_NODE_CACHE;

    u32 SLAB_MIN_ORDER;
    u32 SLAB_MAX_ORDER;
    u32 SLAB_MIN_OBJECTS;

    /// How many objects of |size| bytes fit in a block of 2^|order| pages.
    ///
    /// The block is BIT(order) pages, not |order| pages -- the two differ for
    /// every order but one, and at order 0 the wrong form yields zero objects,
    /// which would make every slab look full the moment it was created.
    FORCE_INLINE
    auto calculate_order_objects(u32 order, u32 size) -> u32 {
        return static_cast<u32>((BIT(order) << PAGE_SHIFT) / size);
    }

    /// The smallest page order whose block is at least |nr_bytes| long.
    ///
    /// get_order() counts frames, not bytes (see pmm.hpp), so the conversion
    /// has to happen here. Feeding it a byte count directly overstates the
    /// order by PAGE_SHIFT and asks the buddy allocator for thousands of pages
    /// to hold a handful of small objects.
    FORCE_INLINE
    auto order_for_bytes(u32 nr_bytes) -> u32 {
        auto const nr_frames = ustl::mm::align_up(usize(nr_bytes), usize(PAGE_SIZE)) >> PAGE_SHIFT;
        return static_cast<u32>(get_order(nr_frames));
    }

    auto calculate_order(u32 size) -> u32 {
        auto min_objects = SLAB_MIN_OBJECTS;
        if (!min_objects) {
            auto const nr_cpus = Cpu::num_present_cpus();
            min_objects = static_cast<u32>(sizeof(usize) * ustl::bit_ceil(nr_cpus));
        }
        auto max_objects = std::max<u32>(calculate_order_objects(SLAB_MAX_ORDER, size), 1);
        min_objects = std::min(min_objects, max_objects);

        // TODO(SmallHuaZi): incomplete -- Linux's slub picks the best order by
        // trying candidates between min_order and SLAB_MAX_ORDER. This
        // returns the lower bound it just computed.
        //
        // Note that SLAB_MIN_ORDER/SLAB_MAX_ORDER/SLAB_MIN_OBJECTS are
        // never assigned anywhere yet, so they are all zero: max_objects caps
        // at one page's worth and every slab comes out order 0. That is a safe
        // place to sit, and raising SLAB_MAX_ORDER later is what the tail
        // stamping in slab::create exists to make survivable.
        return std::max<u32>(SLAB_MIN_ORDER, order_for_bytes(min_objects * size));
    }
} // namespace

namespace slab {

    /// Records, on each tail descriptor of a multi-frame slab, how far it sits
    /// from the head, so that frame_to_folio can walk back from an interior
    /// object address to the descriptor holding the metadata.
    ///
    /// |stamp| false clears them again. Clearing is not optional: the buddy may
    /// hand these frames out separately next time, and a stale offset would send
    /// frame_to_folio walking backwards out of the block it was given.
    ///
    /// A buddy block never straddles a section, so the tails are always the
    /// neighbouring entries of one memmap array and plain indexing reaches them.
    static auto stamp_compound_offsets(Slab *head, usize order, bool stamp) -> void {
        auto const nr_frames = BIT(order);
        for (usize i = 1; i < nr_frames; ++i) {
            head[i].set_compound_offset(stamp ? static_cast<u32>(i) : 0U);
        }
    }

    auto destroy(Slab *slab) -> void {
        DEBUG_ASSERT(slab->as<Body>().num_inuse == 0);

        slab->put();
        slab->unlink();
        auto const order = slab->order();
        stamp_compound_offsets(slab, order, false);
        // Handed back still tagged Slab. The buddy allocator reads Role::Pmm as
        // "this frame is on my free list" (FrameBuddy::is_mergeable()), and only
        // its own insert_frame(), under the zone lock, may say so. This used to
        // stamp Role::Pmm here, through a PmmBody type since deleted, before
        // free_frames() had even reached the lock:
        // for that window the frame claimed to be free while on no list, and a
        // CPU releasing its buddy meanwhile coalesced it -- erasing a node that
        // was never linked, which the intrusive list traps on. Seen as a #UD in
        // FrameBuddy::release_inner() under dbg_1. Every other caller of
        // free_frames() hands frames back in the role they were used in.
        mm::free_frames(slab, order);
    }

    /// Where a free object stores the pointer to the next one.
    ///
    /// Not necessarily offset zero: compute_layout() places it around the middle
    /// of the object, which is what catches an overflow of the preceding object
    /// rather than letting it quietly rewrite a list pointer.
    FORCE_INLINE
    auto free_slot(void *object, usize next_offset) -> void ** {
        return reinterpret_cast<void **>(static_cast<u8 *>(object) + next_offset);
    }

    auto get_object(Slab *slab) -> void * {
        auto &body = slab->as<Body>();
        auto *const object = body.free_head;
        if (!object) {
            return nullptr;
        }

        // The free list threads through the objects themselves, null at the end.
        body.free_head = *free_slot(object, body.next_offset);
        body.num_inuse += 1;
        return object;
    }

    auto put_object(Slab *slab, void *object) -> void {
        auto &body = slab->as<Body>();
        *free_slot(object, body.next_offset) = body.free_head;
        body.free_head = object;
        body.num_inuse -= 1;
    }

    auto init(Slab *slab, MemCache *owner, void *node_cache, usize size, usize next_offset)
        -> Status {
        // Free objects carry the list pointer inside themselves, so the slot has
        // to fit within the stride. compute_layout() keeps next_offset near the
        // middle of the user size, and rounds the stride up to a word.
        DEBUG_ASSERT(next_offset + sizeof(void *) <= size,
                     "the free-list pointer does not fit inside an object");

        auto &body = slab->as<Body>();
        body.owner = owner;
        body.node_cache = node_cache;
        body.next_offset = static_cast<u16>(next_offset);

        auto const capacity = slab->phys_size() / size;
        if (!capacity) {
            return Status::InvArgs;
        }

        // Thread the free list through the objects themselves, walking back to
        // front so the head ends up being the lowest address.
        auto *const base = reinterpret_cast<u8 *>(frame_to_virt(slab));
        void *head = nullptr;
        for (auto i = capacity; i-- > 0;) {
            auto *const object = base + (i * size);
            *free_slot(object, next_offset) = head;
            head = object;
        }

        body.free_head = head;
        body.num_objects = static_cast<u16>(capacity);
        body.num_inuse = 0;
        return Status::Ok;
    }

    auto allocate(Slab *slab, Gaf gaf) -> void * {
        return get_object(slab);
    }

    auto create(MemCache *owner, void *node_cache, Gaf gaf, usize order, usize size,
                usize next_offset, NnId nid) -> Slab * {
        auto frame = mm::alloc_frames(nid, gaf, order);
        if (!frame) {
            return nullptr;
        }

        // Claim the fresh frame for the slab role: clears the payload and stamps
        // the tag in one step.
        frame->become<Body>();
        stamp_compound_offsets(frame, order, true);
        if (auto status = init(frame, owner, node_cache, size, next_offset); Status::Ok != status) {
            stamp_compound_offsets(frame, order, false);
            // Not stamped Pmm here either; see destroy().
            mm::free_frames(frame, order);
            return nullptr;
        }

        frame->get();
        return frame;
    }

} // namespace slab

struct MemCache::CpuCache {
    static auto on_mem_cache_init(MemCache &cache) -> Status;
    static auto on_mem_cache_free(MemCache &cache) -> void;

    /// The slab this cpu is currently allocating from, or null.
    auto active_slab() -> Slab *;

    /// Stops allocating from |slab|. Deliberately does *not* touch its list
    /// membership: that belongs to the node lock alone (see NodeCache).
    auto clear_active(Slab *slab) -> void;

    /// Takes |slab| as the active one and returns whichever slab it displaced,
    /// for the caller to park. Returning it instead of parking it here keeps
    /// the cpu lock from ever nesting inside the node lock.
    [[nodiscard]]
    auto install_active(Slab *slab) -> Slab *;

    /// Cursor into the active slab's null-terminated free list.
    void *m_objects;
    usize transaction_id;

    Slab *active;

    // There is no per-cpu partial list, and that is a deliberate retreat.
    // Two partial lists under two different locks make "which list is this
    // slab on" a single bit of state guarded by neither: a cpu unlinking a
    // full slab from its own list races with another cpu's free() deciding,
    // under the node lock, that the slab is already linked and needs no
    // parking. The slab then ends up on no list at all with free objects in
    // it -- leaked. Bringing the per-cpu list back needs a per-slab lock or a
    // cmpxchg_double on (freelist, counters) the way slub does it.

    [[no_unique_address]]
    sync::LocalLock lock;
};

auto MemCache::CpuCache::on_mem_cache_init(MemCache &cache) -> Status {
    auto cpu_cache = CpuLocal::allocate<CpuCache>(1, alignof(CpuCache));
    if (!cpu_cache) {
        return Status::NoMemory;
    }

    cpu_cache.for_each([](CpuNum cpu, CpuCache &self) { self.transaction_id = cpu; });

    cache.m_cpu_caches = ustl::move(cpu_cache);
    return Status::Ok;
}

auto MemCache::CpuCache::on_mem_cache_free(MemCache &cache) -> void {
    if (!cache.m_cpu_caches) {
        return;
    }

    // A cpu's active slab is on no list, so once the per-cpu areas are gone
    // nothing refers to it any more. Hand the frames back before that happens.
    cache.m_cpu_caches.for_each([](CpuNum, CpuCache &self) {
        if (self.active) {
            slab::destroy(self.active);
            self.active = nullptr;
            self.m_objects = nullptr;
        }
    });

    CpuLocal::free(ustl::move(cache.m_cpu_caches));
}

auto MemCache::CpuCache::active_slab() -> Slab * {
    Guard<sync::LocalLock, IrqSave> guard{lock};
    return active;
}

auto MemCache::CpuCache::clear_active(Slab *slab) -> void {
    Guard<sync::LocalLock, IrqSave> guard{lock};
    if (active == slab) {
        active = nullptr;
        m_objects = nullptr;
    }
}

auto MemCache::CpuCache::install_active(Slab *slab) -> Slab * {
    Guard<sync::LocalLock, IrqSave> guard{lock};
    auto *const displaced = active == slab ? nullptr : active;

    active = slab;
    m_objects = nullptr;
    return displaced;
}

/// The shared pool of partially used slabs for one node, and the lock that
/// makes the whole slab layer safe.
///
/// Every mutation of a slab -- its free list and whether it is on a list at
/// all -- happens under the lock of the node cache recorded in the slab
/// itself. That is the point: two cpus freeing into the same slab read the
/// same pointer out of it and therefore contend on the same lock, which is
/// what a per-cpu lock could never give.
struct MemCache::NodeCache {
    static auto on_mem_cache_init(MemCache &cache) -> Status;
    static auto on_mem_cache_free(MemCache &cache) -> void;

    /// Takes one object out of |slab|, or null if it has none left.
    ///
    /// |exhausted| reports, under the same lock, whether the slab has just run
    /// out. The caller must not ask separately: between the two a free() could
    /// park the slab, and installing a listed slab as a cpu's active one hands
    /// the same slab to two cpus at once.
    auto take_object(Slab *slab, bool *exhausted) -> void *;

    /// Detaches one slab from the partial list, or null if it is empty.
    auto take_partial() -> Slab *;

    /// Puts |slab| on the partial list unless it is already on it.
    auto park(Slab *slab) -> void;

    /// Held with interrupts off, like SLUB's kmem_cache_node::list_lock
    /// (spin_lock_irqsave in get_partial_node() and __slab_free()): the
    /// per-CPU caches above it are already IrqSave, and a free reaching this
    /// list from an interrupt must not meet it held underneath.
    sync::SpinLock lock;
    Slab::List partials;
    usize num_partials;

    /// NodeCaches for the node cache itself, which cannot allocate its own:
    /// allocating a NodeCache goes through NODE_CACHE, and during its
    /// bootstrap that *is* the cache being built. Static for the same reason
    /// CpuLocal keeps its bootstrap areas static -- the slab cannot be used to
    /// build what the slab depends on.
    static NodeCache BOOTSTRAP[NUMA_MAX_NODE];

    static auto is_bootstrap(NodeCache const *cache) -> bool {
        return cache >= &BOOTSTRAP[0] && cache < &BOOTSTRAP[NUMA_MAX_NODE];
    }
};

MemCache::NodeCache MemCache::NodeCache::BOOTSTRAP[NUMA_MAX_NODE];

auto MemCache::NodeCache::take_object(Slab *slab, bool *exhausted) -> void * {
    Guard<sync::SpinLock, IrqSave> guard{lock};

    auto *const object = slab::get_object(slab);
    *exhausted = !slab::has_object(slab);
    return object;
}

auto MemCache::NodeCache::take_partial() -> Slab * {
    Guard<sync::SpinLock, IrqSave> guard{lock};

    auto front = partials.front();
    if (!front) {
        return nullptr;
    }

    auto *const slab = &*front;
    partials.pop_front();
    num_partials -= 1;
    return slab;
}

auto MemCache::NodeCache::park(Slab *slab) -> void {
    Guard<sync::SpinLock, IrqSave> guard{lock};
    if (slab->is_linked()) {
        return;
    }

    partials.push_back(*slab);
    num_partials += 1;
}

auto MemCache::NodeCache::on_mem_cache_init(MemCache &cache) -> Status {
    // The node cache is brought up on static storage; everything afterwards,
    // the self-cache included, allocates through it normally.
    auto const bootstrapping = &cache == NODE_CACHE;

    auto const &node = SLAB_NODES;
    for (usize nid = 0; nid < node.size(); ++nid) {
        if (!node.test(nid)) {
            continue;
        }

        if (bootstrapping) {
            cache.m_node_caches[nid] = &BOOTSTRAP[nid];
            continue;
        }

        auto *const storage = NODE_CACHE->allocate(cache.m_gaf, static_cast<NnId>(nid));
        if (!storage) {
            // Release all holding and report error.
            on_mem_cache_free(cache);
            return Status::NoMemory;
        }

        cache.m_node_caches[nid] = new (storage) NodeCache();
    }

    return Status::Ok;
}

auto MemCache::NodeCache::on_mem_cache_free(MemCache &cache) -> void {
    // Cleared as we go: this runs both from on_mem_cache_init's failure path
    // and again from ~MemCache, and the second pass would otherwise free every
    // pointer a second time.
    for (auto &node_cache: cache.m_node_caches) {
        if (!node_cache) {
            continue;
        }

        // Drain the pool first: those slabs are reachable from nowhere else,
        // so letting the NodeCache go without them leaks the frames.
        while (auto *const slab = node_cache->take_partial()) {
            slab::destroy(slab);
        }

        // The bootstrap NodeCaches are static storage; they were never
        // allocated and must not be handed to the allocator.
        if (!is_bootstrap(node_cache)) {
            node_cache->~NodeCache();
            NODE_CACHE->free(node_cache);
        }
        node_cache = nullptr;
    }
}

constexpr static auto MAX_PARTIAL_SLABS = 8U;

constexpr static auto MIN_PARTIAL_SLABS = 4U;

[[maybe_unused]]
constexpr static auto RED_INACTIVE = 0xccU;

[[maybe_unused]]
constexpr static auto RED_ACTIVE = 0xddU;

[[maybe_unused]]
constexpr static auto POISON_INUSE = 0x5aU;

[[maybe_unused]]
constexpr static auto POISON_FREE = 0x6bU;

[[maybe_unused]]
constexpr static auto POISON_END = 0xa5U;

// Out-of-line definitions for MemCache's static data members. The header only
// declares them, so without these every use -- and there are many, this file
// locks CACHE_LOCK and reads SLAB_NODES throughout -- is an undefined
// symbol that only the link would ever complain about.
sync::Mutex MemCache::CACHE_LOCK;
MemCache::List MemCache::GLOBAL_CACHES;
NodeMask MemCache::SLAB_NODES;

MemCache::MemCache(char const *name, usize size, usize align, Option option)
    // Initializer order follows the member declaration order in the header.
    //
    // Everything compute_layout() derives is zeroed here rather than left
    // indeterminate. m_user_align in particular used to be read by
    // compute_layout without ever having been written by anyone -- and it is
    // read as an alignment, so a garbage value either skewed the object stride
    // or, at zero, collapsed it to nothing and divided by it.
    : m_name(name)
    , m_option(option)
    , m_phys_size(0)
    , m_user_size(static_cast<u32>(size))
    , m_next_offset(0)
    , m_user_align(static_cast<u32>(align ? align : alignof(usize)))
    , m_inuse(0)
    , m_red_left_pad(0)
    , m_min_partial(MIN_PARTIAL_SLABS)
    , m_num_objects(0)
    , m_order(0)
    , m_min_num_objects(0)
    , m_min_order(0)
    , m_gaf(Gaf::Compound)
    // std::array of raw pointers: without this the entries are indeterminate,
    // and ~MemCache walks every one of them and frees it.
    , m_node_caches{} {
    // Every MemCache instance's storage comes from the self-cache, so the
    // refcount machinery must reclaim it there instead of `delete`.
    set_deallocate(&Self::reclaim_storage);
}

auto MemCache::reclaim_storage(void *pointer) noexcept -> void {
    SELF_CACHE->free(pointer);
}

MemCache::~MemCache() noexcept {
    CpuCache::on_mem_cache_free(*this);
    NodeCache::on_mem_cache_free(*this);
}

auto MemCache::init() -> Status {
    SELF_CACHE = BOOTSTRAP_SELF_CACHE.data();
    NODE_CACHE = BOOTSTRAP_NODE_CACHE.data();

    // Initialize the nodemask to nodes which has normal memories.
    PmNode::for_each_nid(PmNode::State::Memory, [](NnId nid) { SLAB_NODES.set(nid); });

    if (auto const status =
            Self::create_boot(*NODE_CACHE, "cache:node", sizeof(NodeCache), 0, Option::CacheAlign);
        Status::Ok != status) {
        return status;
    }
    return Self::create_boot(*SELF_CACHE, "cache:self", sizeof(MemCache), 0, Option::CacheAlign);
}

auto MemCache::compute_layout(usize size, Option option) -> Status {
    constexpr auto WORD_SIZE = sizeof(void *);

    // At least keep a size of FP.
    usize phys_size = ustl::mm::align_up(size, WORD_SIZE);

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        // Check if there is left spaces for readzone between the end of object and
        // word boundaries. May it be the readzone rather than additional spaces.
        if (!!(option & Option::RedZone) && phys_size == m_user_size) {
            phys_size += WORD_SIZE + size;
        }
    }

    m_inuse = static_cast<u32>(phys_size);

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        // Under debug mode and debug options enabled, only placing the FP after
        // user object is permitted.
        if (!!(option & Option::Poison) ||
            (!!(option & Option::RedZone) && m_user_size < phys_size)) {
            m_next_offset = static_cast<u32>(phys_size);
            phys_size += WORD_SIZE; // Put the FP
        } else {
            m_next_offset = ustl::mm::align_down(m_user_size / 2, WORD_SIZE);
        }
    } else {
        // Default case, we store freelist pointer near middle of object.
        m_next_offset = ustl::mm::align_down(m_user_size / 2, WORD_SIZE);
    }

    if constexpr (CONFIG_MEM_CACHE_DEBUG) {
        if (!!(option & Option::RedZone)) {
            m_red_left_pad = static_cast<u32>(ustl::mm::align_up(WORD_SIZE, m_user_align));
            phys_size += m_red_left_pad;
        }
    }

    phys_size = ustl::mm::align_up(phys_size, m_user_align);

    auto const order = calculate_order(static_cast<u32>(phys_size));
    if (static_cast<i32>(order) < 0) {
        return Status::InternalError;
    }

    m_phys_size = static_cast<u32>(phys_size);
    m_order = static_cast<u16>(order);
    // Both counts go through the same helper so they cannot drift apart again.
    m_num_objects = static_cast<u16>(calculate_order_objects(order, m_phys_size));
    m_min_order = static_cast<u16>(order_for_bytes(m_phys_size));
    m_min_num_objects = static_cast<u16>(calculate_order_objects(m_min_order, m_phys_size));
    return Status::Ok;
}

auto MemCache::init(char const *name, usize size, usize align, Option option) -> Status {
    if (auto const status = compute_layout(size, option); Status::Ok != status) {
        return status;
    }

    m_min_partial =
        std::clamp(ustl::bit_ceil(m_phys_size) / 2, MIN_PARTIAL_SLABS, MAX_PARTIAL_SLABS);

    // CpuCache first, and the order matters. NodeCache::on_mem_cache_init()
    // allocates its NodeCaches out of NODE_CACHE -- and when the cache being
    // brought up *is* NODE_CACHE, that is a call into this very object's
    // allocate(), which reads m_cpu_caches. Setting the cpu caches up first is
    // what lets the node cache carve its own NodeCaches out of a fresh slab
    // instead of chasing an uninitialised member.
    //
    // CpuCache has no matching dependency: CpuLocal keeps its own arena
    // precisely so that the slab can be built on top of it.
    if (auto const status = CpuCache::on_mem_cache_init(*this); Status::Ok != status) {
        return status;
    }

    if (auto const status = NodeCache::on_mem_cache_init(*this); Status::Ok != status) {
        return status;
    }

    {
        Guard guard{CACHE_LOCK};
        GLOBAL_CACHES.push_back(*this);
    }
    return Status::Ok;
}

auto MemCache::alias(const char *name, usize size, usize align, Option, ustl::Arc<Self> *)
    -> Status {
    // TODO(SmallHuaZi): unimplemented -- nothing creates an aliasing cache
    // yet.
    return Status::Unimplemented;
}

auto MemCache::create(char const *name, usize size, usize align, Option option,
                      ustl::Arc<Self> *out) -> Status {
    // No CACHE_LOCK here. It guards GLOBAL_CACHES, which init() below
    // appends to under that same lock -- and Mutex is a plain CAS lock that
    // stores the owning thread, so taking it here deadlocked the moment the
    // first cache was ever created. Allocating the storage needs no such
    // protection: MemCache::allocate does its own locking.
    auto raw_self = SELF_CACHE->allocate(GAF_KERNEL);
    if (!raw_self) {
        return Status::NoMemory;
    }

    auto cleanup = gktl::defer([raw_self]() { SELF_CACHE->free(raw_self); });

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
    // |self| is raw storage. The two bootstrap caches live in LazyInit slots
    // that were deliberately never constructed, because constructing one the
    // ordinary way would allocate from the very allocator this is bringing up.
    //
    // No CACHE_LOCK here: init() takes it for the one line that needs it,
    // and Mutex is not recursive.
    auto *const cache = new (&self) Self(name, size, align, options);
    return cache->init(name, size, align, options);
}

auto MemCache::allocate(Gaf gaf, NnId nid, CpuCache *cache) -> void * {
    DEBUG_ASSERT(nid < NUMA_MAX_NODE, "caller must resolve 'no preference' first");

    auto *const node_cache = m_node_caches[nid];
    if (!node_cache) [[unlikely]] {
        // This cache keeps no slabs on that node.
        return nullptr;
    }

    auto exhausted = false;

    // 1. The slab this cpu is already working from. Nobody else can take from
    //    it -- it is on no list -- so it cannot go full behind our back.
    if (auto *const slab = cache->active_slab()) {
        if (auto *const object = node_cache->take_object(slab, &exhausted)) {
            if (exhausted) {
                // Stop holding it, and leave it on no list. free() recognises
                // a full slab precisely by its being unlisted, and that is how
                // it gets back into circulation.
                cache->clear_active(slab);
            }
            return object;
        }

        cache->clear_active(slab);
    }

    // 2. The node's shared pool, and failing that the buddy allocator.
    auto *slab = node_cache->take_partial();
    if (!slab) {
        slab =
            slab::create(this, node_cache, m_gaf | gaf, m_order, m_phys_size, m_next_offset, nid);
        if (!slab) {
            return nullptr;
        }
    }

    auto *const object = node_cache->take_object(slab, &exhausted);
    DEBUG_ASSERT(object, "a slab off the partial list had nothing to give");

    // Only adopt it if it can serve again. A full slab must stay unlisted and
    // unowned, which is the invariant free() reads.
    if (!exhausted) {
        if (auto *const displaced = cache->install_active(slab)) {
            node_cache->park(displaced);
        }
    }

    return object;
}

auto MemCache::allocate(Gaf gaf, NnId nid) -> void * {
    task::AutoPreemptDisabler preempt_disabler{};

    // NUMA_MAX_NODE is how a caller spells "no preference": it is this
    // function's default argument, and ObjectCache::get() always passes it
    // explicitly. Resolve it to a real node here, while preemption is already
    // disabled so current_node() cannot change under us.
    //
    // Passing it through was a live out-of-bounds read. The inner overload
    // maps a node it cannot use onto NUMA_NO_NODE, which is defined as
    // NUMA_MAX_NODE + 1, and then indexes m_node_caches -- an array exactly
    // NUMA_MAX_NODE long -- with it. It also tripped that overload's own
    // DEBUG_ASSERT(nid < NUMA_MAX_NODE) on the way in.
    if (nid >= NUMA_MAX_NODE) {
        nid = current_node();
    }
    DEBUG_ASSERT(nid < NUMA_MAX_NODE, "current_node() returned an unusable node");

    return allocate(gaf, nid, m_cpu_caches.ptr_mut());
}

auto MemCache::free(void *object) -> void {
    if (!object) {
        return;
    }

    // The object pointer may be anywhere inside the slab, so resolve it to the
    // head descriptor, which is where the metadata lives.
    auto *const host = virt_to_folio(object);
    DEBUG_ASSERT(host, "freeing an address that is outside the physmap");
    DEBUG_ASSERT(host->try_as<slab::Body>(), "freeing an address that no slab backs");
    DEBUG_ASSERT(slab::owner(host) == this, "object returned to a cache that does not own it");

    auto *const node_cache = static_cast<NodeCache *>(slab::node_cache(host));
    DEBUG_ASSERT(node_cache, "slab without an owning node cache");

    Slab *to_destroy = nullptr;
    {
        Guard<sync::SpinLock, IrqSave> guard{node_cache->lock};

        auto const was_full = !slab::has_object(host);

        slab::put_object(host, object);

        if (was_full) {
            // It was on no list and nobody's active slab. Returning an object
            // is what makes it usable again, so this is where it rejoins the
            // pool -- otherwise the memory would be free but unreachable.
            if (!host->is_linked()) {
                node_cache->partials.push_back(*host);
                node_cache->num_partials += 1;
            }
        } else if (slab::is_empty(host) && host->is_linked() &&
                   node_cache->num_partials > m_min_partial) {
            // Wholly unused, and the node has spare partial slabs, so give the
            // memory back instead of hoarding it. The is_linked() test is what
            // keeps a cpu's active slab -- which is never on a list -- from
            // being destroyed out from under it.
            host->unlink();
            node_cache->num_partials -= 1;
            to_destroy = host;
        }
    }

    if (to_destroy) {
        // Outside the node lock on purpose: this walks into the buddy
        // allocator, which takes locks of its own.
        slab::destroy(to_destroy);
    }
}

} // namespace ours::mm