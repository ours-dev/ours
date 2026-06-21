/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_COW_PAGES_HPP
#define OURS_MEM_VM_COW_PAGES_HPP 1

#include <ours/sync/mutex.hpp>

#include <ours/mem/gaf.hpp>
#include <ours/mem/vm_page_map.hpp>
#include <ours/mem/page-request.hpp>

#include <ustl/rc.hpp>

namespace ours::mem {
    /// A group of the copy on write page.
    class VmCowPages: public ustl::RefCounter<VmCowPages> {
        using Self = VmCowPages;
      public:
        class Cursor;

        static auto create(Gaf gaf, usize size, ustl::Rc<VmCowPages> *out) -> Status;

        static auto with_source(ustl::Rc<PageSource> src, Gaf gaf, usize size, ustl::Rc<VmCowPages> *out) -> Status;

        auto commit_range_locked(VirtAddr offset, usize size, usize *nr_commited) -> Status;

        auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<Cursor, Status>;

        FORCE_INLINE
        auto size_locked() const -> usize { return m_size; }
      private:
        VmCowPages(Gaf gaf, usize num_pages);

        ~VmCowPages();

        auto alloc_pages(usize order, VmPage **page, PageRequest *page_request) -> Status;

        /// When no page sources exists, it will be used in frame allocation request.
        Gaf m_gaf;
        /// Page map that operates within the virtual memory range of the VMO, from [0, N).
        VmPageMap m_pages;
        ustl::Rc<PageSource> m_page_source;
        usize m_size;
    };

    class VmCowPages::Cursor {
        using Self = VmCowPages::Cursor;
      public:
        Cursor(VmCowPages *owner, VirtAddr offset, usize size);
        ~Cursor() = default;

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

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_COW_PAGES_HPP