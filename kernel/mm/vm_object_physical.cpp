#include <ours/mm/vm_object_physical.hpp>

#include <heap/new.hpp>

namespace ours::mm {
VmObjectPhysical::VmObjectPhysical(PhysAddr base, usize size, VmoFlags flags)
    : Base(gktl::magic<Self>(), VmObject::Type::Physical, flags, m_own_lock)
    , m_phys_base(base)
    , m_size(size) {}

auto VmObjectPhysical::create(PhysAddr base, usize size, Flags vmof, ustl::Arc<Self> *out)
    -> Status {
    if (!size) {
        return Status::InvArgs;
    }
    if (base == PhysAddr{}) {
        return Status::InvArgs;
    }

    auto self = new (GAF_KERNEL) VmObjectPhysical(base, size, vmof);
    if (!self) {
        return Status::OutOfMem;
    }

    *out = ustl::Arc<Self>::adopt(self);
    return Status::Ok;
}

} // namespace ours::mm