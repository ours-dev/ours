#include <heap/kmalloc.hpp>

#include <array>

#include <ours/assert.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/platform/init.hpp>
#include <ustl/bit.hpp>
#include <ustl/lazy_init.hpp>

// MAX_NODES was a macro, so it needed no namespace; it is an inline
// constexpr in <ours/node_mask.hpp> now, and this file is in `heap`.
using ours::MAX_NODES;
using ours::Status;
using ours::mm::MemCache;

using namespace ours::mm;

namespace heap {
// The ladder itself is declared in <heap/kmalloc.hpp>: `operator new` needs
// the upper bound to know when to route to vmalloc instead, so it cannot stay
// private to this file.
constexpr static auto NUM_MALLOC_CACHES = KMALLOC_NUM_CACHES;

constexpr static auto MALLOC_MIN_ORDER = KMALLOC_MIN_ORDER;

constexpr static auto MIN_MALLOC_SIZE = KMALLOC_MIN_SIZE;

constexpr static auto MAX_MALLOC_SIZE = KMALLOC_MAX_SIZE;

/// Names outlive the caches on purpose: MemCache stores the pointer it is
/// given and never copies the string, so anything with automatic storage
/// would dangle as soon as init_kmalloc() returned.
constexpr static char const *MALLOC_CACHE_NAMES[NUM_MALLOC_CACHES] = {
    "kmalloc-8",   "kmalloc-16",  "kmalloc-32",   "kmalloc-64",   "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024", "kmalloc-2048", "kmalloc-4096",
};

static auto get_allocation_index(usize size) -> usize {
    DEBUG_ASSERT(size <= MAX_MALLOC_SIZE, "size has no kmalloc size class");

    if (size <= MIN_MALLOC_SIZE) {
        return 0;
    }

    // Round up to the next power of two and index by its log. Clamped rather
    // than subtracted blindly: for a size below the smallest class the width
    // is under MALLOC_MIN_ORDER and the difference would wrap.
    auto const width = ustl::bit_width<usize>(size - 1);
    return width - MALLOC_MIN_ORDER;
}

// Size-class caches are typeless by nature -- they serve raw byte sizes, not
// one particular type -- so they use MemCache directly rather than
// ObjectCache. LazyInit gives static storage without running MemCache's
// private constructor; create_boot() initializes in place, the same pattern
// MemCache::init() uses for its own bootstrap caches.
static std::array<ustl::LazyInit<MemCache>, NUM_MALLOC_CACHES> KMALLOC_CACHE;

auto kmalloc(usize size, AllocFlags alloc_flags, NodeId nid) -> void * {
    if (size > MAX_MALLOC_SIZE) {
        // Now we do not support to allocate frames from kmalloc
        return nullptr;
    }
    if (nid == MAX_NODES) {
        nid = current_node();
    }

    auto const index = get_allocation_index(size);
    return KMALLOC_CACHE[index]->allocate(alloc_flags, nid);
}

auto kmalloc(usize size, AllocFlags alloc_flags) -> void * {
    if (size > MAX_MALLOC_SIZE) {
        // Now we do not support to allocate frames from kmalloc
        return nullptr;
    }

    auto const index = get_allocation_index(size);
    return KMALLOC_CACHE[index]->allocate(alloc_flags, current_node());
}

auto kfree(void *object) -> void {
    if (!object) {
        return;
    }

    // kfree is handed a bare pointer of unknown provenance, so this is a role
    // *query*: try_as() returns null unless a slab really backs the frame,
    // and nothing here rewrites the role tag.
    auto *const host = virt_to_folio(object);
    DEBUG_ASSERT(host, "kfree() on an address that is outside the physmap");
    DEBUG_ASSERT(host->try_as<page::SlabBody>(), "kfree() on an address that no slab backs");

    auto *const cache = static_cast<MemCache *>(host->slab_owner());
    DEBUG_ASSERT(cache, "slab has no owning cache");
    cache->free(object);
}

INIT_CODE
auto init_kmalloc() -> void {
    for (usize i = 0; i < KMALLOC_CACHE.size(); ++i) {
        // data(), not init(): create_boot() initializes the cache in place,
        // exactly as MemCache::init() does for its own bootstrap caches.
        // Calling init() would need a default constructor MemCache does not
        // have -- its constructors are private and take the layout up front.
        [[maybe_unused]]
        auto const status = MemCache::create_boot(*KMALLOC_CACHE[i].data(), MALLOC_CACHE_NAMES[i],
                                                  usize{1} << (i + MALLOC_MIN_ORDER),
                                                  alignof(usize), MemCache::Option::CacheAlign);
        DEBUG_ASSERT(Status::Ok == status);
    }
}

} // namespace heap
