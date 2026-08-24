#include <ktl/new.hpp>
#include <ours/mm/vm_object_physical.hpp>

namespace ours::mm {
VmObjectPhysical::VmObjectPhysical(PhysAddr base, usize size, VmoFlags VmoFlags)
    : Base(VmObject::Type::Physical, VmoFlags), phys_base_(base), size_(size) {}

auto VmObjectPhysical::create(PhysAddr base, usize size, Flags vmof, ustl::Arc<Self> *out)
    -> Status {
    if (!size) {
        return Status::InvalidArguments;
    }
    if (!base) {
        return Status::InvalidArguments;
    }

    auto self = new (kGafKernel) VmObjectPhysical(base, size, vmof);
    if (!self) {
        return Status::OutOfMem;
    }

    *out = ustl::Arc<Self>::adopt(self);
    return Status::Ok;
}

} // namespace ours::mm