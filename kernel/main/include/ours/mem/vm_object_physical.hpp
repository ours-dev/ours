/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_VM_OBJECT_PHYSICAL_HPP
#define OURS_MEM_VM_OBJECT_PHYSICAL_HPP 1

#include <ours/mem/vm_object.hpp>

namespace ours::mem {
    /// The object to describe a section of physical memory (Unpaged RAM fragment, Device Memory and e.g.)
    class VmObjectPhysical: public VmObject {
        using Self = VmObjectPhysical;
        using Base = VmObject;
      public:
        static auto create(PhysAddr pa, usize size, Flags VmoFlags, ustl::Rc<Self> *out) -> Status;

        /// 
        virtual auto commit_range(VirtAddr offset, usize size, CommitOptions option) -> Status override {
            m_canary.verify(kPhysicalMagic);

            if (offset > size_ || size > size_ || offset > size_ - size) {
                return Status::OutOfRange;
            }
            return Status::Ok;
        }

        auto lookup_range(usize offset, usize size, PhysAddr *pa) -> Status {
            m_canary.verify(kPhysicalMagic);

            if (offset > size_ || size > size_ || offset > size_ - size) {
                return Status::OutOfRange;
            }

            *pa = phys_base_ + offset;
            return Status::Ok;
        }

      private:
        VmObjectPhysical(PhysAddr pa, usize size, Flags VmoFlags);

        PhysAddr phys_base_;
        usize size_;
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_VM_OBJECT_PHYSICAL_HPP