/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_VM_OBJECT_PHYSICAL_HPP
#define OURS_MM_VM_OBJECT_PHYSICAL_HPP 1

#include <ours/mm/vm_object.hpp>

namespace ours::mm {
/// The object to describe a section of physical memory (Unpaged RAM fragment, Device Memory and e.g.)
class VmObjectPhysical: public VmObject {
    using Self = VmObjectPhysical;
    using Base = VmObject;

  public:
    static auto create(PhysAddr pa, usize size, Flags flags, ustl::Arc<Self> *out) -> Status;

    ///
    auto commit_range(usize offset, usize size, CommitOptions option) -> Status override {
        m_canary.verify<Self>();

        if (!check_range(offset, size)) {
            return Status::OutOfRange;
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto size() const -> usize override {
        return m_size;
    }

    /// A window onto physical memory is contiguous by being one: the address of
    /// its byte at |offset|. The same range checks as lookup_range(); a window
    /// that does not start on a page can still be asked for a run of its own
    /// bytes, so the alignment the paged object demands is not asked here.
    auto lookup_contiguous(usize offset, usize size, PhysAddr *out) -> Status override {
        if (!size) {
            return Status::InvArgs;
        }
        return lookup_range(offset, size, out);
    }

    auto lookup_range(usize offset, usize size, PhysAddr *pa) -> Status {
        m_canary.verify<Self>();

        if (!check_range(offset, size)) {
            return Status::OutOfRange;
        }

        *pa = m_phys_base + offset;
        return Status::Ok;
    }

  private:
    VmObjectPhysical(PhysAddr pa, usize size, Flags flags);

    auto check_range(usize offset, usize size) const -> bool {
        return !(offset > m_size || size > m_size || offset > m_size - size);
    }

    PhysAddr m_phys_base;
    usize m_size;

    /// The lock VmObject::lock() answers for this object. Its own, because
    /// a physical VMO shares no pages with anything, so there is no tree's
    /// lock for it to take. The base is handed a reference to it before it is
    /// constructed, which is sound: the base only stores the reference, and
    /// nothing locks it until the object is whole.
    sync::Mutex m_own_lock;
};

} // namespace ours::mm

#endif // #ifndef OURS_MM_VM_OBJECT_PHYSICAL_HPP
