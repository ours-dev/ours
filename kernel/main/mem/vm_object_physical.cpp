#include <ours/mem/vm_object_physical.hpp>
#include <ktl/new.hpp>

namespace ours::mem {
    VmObjectPhysical::VmObjectPhysical(PhysAddr base, usize size, VmoFlags VmoFlags)
        : Base(VmObject::Type::Physical, VmoFlags),
          phys_base_(base), size_(size)
    {}

    auto VmObjectPhysical::create(PhysAddr base, usize size, Flags vmof, ustl::Rc<Self> *out) -> Status {
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

        *out = ustl::make_rc<Self>(self);
        return Status::Ok;
    }

} // namespace ours::mem