#include <ours/object/resource_dispatcher.hpp>

namespace ours::object {
ResourceDispatcher::ResourceDispatcher(Kind kind, PhysAddr base, PhysAddr size, Flag flags,
                                       Name const &name)
    // Initializer order follows the member declaration order in the header.
    : Base(), m_name(name), m_kind(kind), m_flags(flags), m_base(base), m_size(size) {}

auto ResourceDispatcher::create(KernelHandle<Self> *out, Rights &rights, Kind kind, PhysAddr base,
                                PhysAddr size, Flag flags, Name const &name, Storage *storage)
    -> Status {
    if (!size) {
        return Status::InvArgs;
    }

    if (!storage) {
        storage = &SYSTEM_RESOURCES;
    }

    if (kind == Kind::Root) {
        if (base || size) {
            return Status::InvArgs;
        }
    }

    return Status::Ok;
}
} // namespace ours::object