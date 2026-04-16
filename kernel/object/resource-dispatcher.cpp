#include <ours/object/resource-dispatcher.hpp>

namespace ours::object {
    ResourceDispatcher::ResourceDispatcher(Kind kind, PhysAddr base, PhysAddr size, 
                                           Flag flags, Name const &name)
        : Base()
        , m_base(base)
        , m_size(size)
        , m_kind(kind)
        , m_flags(flags)
        , m_name(name)
    {}

    auto ResourceDispatcher::create(KernelHandle<Self> *out, Rights &rights, Kind kind, 
                                    PhysAddr base, PhysAddr size, Flag flags, 
                                    Name const &name, Storage *storage) -> Status {
        if (!size) {
            return Status::InvalidArguments;
        }

        if (!storage) {
            storage = &g_system_resources;
        }

        if (kind == Kind::Root) {
            if (base || size) {
                return Status::InvalidArguments;
            }
        }

        return Status::Ok;
    }
} // namespace ours::object