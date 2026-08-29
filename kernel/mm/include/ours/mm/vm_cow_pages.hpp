/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_COW_PAGES_HPP
#define OURS_MM_VM_COW_PAGES_HPP 1

#include <ours/mm/gaf.hpp>
#include <ours/mm/object_cache.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/vm_page_map.hpp>
#include <ours/sync/mutex.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
/// A group of the copy on write page.
class VmCowPages: public ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy> {
    using Self = VmCowPages;

  public:
    class Cursor;

    static auto create(Gaf gaf, usize size, ustl::Arc<VmCowPages> *out) -> Status;

    static auto with_source(ustl::Arc<PageSource> src, Gaf gaf, usize size,
                            ustl::Arc<VmCowPages> *out) -> Status;

    auto commit_range_locked(VirtAddr offset, usize size, usize *nr_commited) -> Status;

    auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status>;

    FORCE_INLINE auto size_locked() const -> usize {
        return m_size;
    }

  private:
    /// RefCounted::destroy() calls ~T() when the last reference goes; the
    /// destructor stays private otherwise, as MemCache and Dispatcher do.
    friend class ustl::RefCounted<VmCowPages, ustl::sync::ThreadSafePolicy>;

    /// ObjectCache::Instance placement-news and destroys T, so it needs the
    /// constructor that stays private to everyone else.
    template <typename, typename...>
    friend class ObjectCache::Instance;

    VmCowPages(Gaf gaf, usize num_pages);

    ~VmCowPages();

    auto alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status;

    /// When no page sources exists, it will be used in frame allocation request.
    Gaf m_gaf;
    /// Page map that operates within the virtual memory range of the VMO, from [0, N).
    VmPageMap m_pages;
    ustl::Arc<PageSource> m_page_source;
    usize m_size;
};

class VmCowPages::Cursor {
    using Self = VmCowPages::Cursor;

  public:
    Cursor(VmCowPages *owner, VirtAddr offset, usize size);
    ~Cursor() = default;

    auto require_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<VmPage *>;
    auto require_read_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<VmPage *>;
    auto require_owned_page(usize nr_pages, MultiPageRequest *request) -> ktl::Result<VmPage *>;

    auto create_read_request(usize nr_pages, PageRequest *request) -> Status;
    auto create_dirty_request(usize nr_pages, PageRequest *request) -> Status;

  private:
    auto convert_reference_to_page(AnonymousPageRequest *request) -> Status;

    auto is_type_reference() const -> bool;
    auto is_type_page() const -> bool;
    auto is_type_zero() const -> bool;

    VmCowPages *m_target;
    usize m_offset;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_COW_PAGES_HPP