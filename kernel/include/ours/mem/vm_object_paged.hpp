/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_OBJECT_PAGED_HPP
#define OURS_MEM_VM_OBJECT_PAGED_HPP 1

#include <ours/mem/vm_object.hpp>
#include <ours/mem/vm_cow_pages.hpp>
#include <ours/sync/mutex.hpp>

#include <logz4/log.hpp>
#include <ustl/result.hpp>

namespace ours::mem {
    class VmObjectPaged: public VmObject {
        using Base = VmObject;
        using Self = VmObjectPaged;
      public:
        static auto create(Gaf gaf, usize size, Flags vmof, ustl::Rc<VmObjectPaged> *out) -> Status;

        static auto create_contiguous(Gaf gaf, usize size, Flags vmof, ustl::Rc<VmObjectPaged> *out) -> Status;

        /// 
        auto commit_range(VirtAddr offset, usize size, CommitOptions option) -> Status override;

        ///
        auto decommit(VirtAddr offset, usize size) -> Status override;

        ///
        auto take_pages(VirtAddr, usize, VmPage::List *) -> Status override;

        ///
        auto supply_pages(VirtAddr, usize, VmPage::List *, SupplyOption) -> Status override;

        ///
        auto read(void *out, VirtAddr offset, usize size) -> Status override;

        ///
        auto write(void *out, VirtAddr offset, usize size) -> Status override;

        FORCE_INLINE
        auto make_cursor(VirtAddr offset, usize size) -> ustl::Result<VmCowPages::Cursor, Status> {
            return m_cow_pages->make_cursor(offset, size);
        }

        /// Priviate on logic, please go to use the facotry methods like VmObjectPaged::create*.
        VmObjectPaged(Flags vmof, ustl::Rc<VmCowPages>);
        virtual ~VmObjectPaged() = default;
      private:
        auto commit_range_internal(PgOff offset, usize n, CommitOptions option) -> Status;

        ustl::Rc<VmCowPages> m_cow_pages;
        Mutex m_mutex;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_OBJECT_PAGED_HPP