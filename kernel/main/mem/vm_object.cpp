#include <ours/mem/vm_object.hpp>

namespace ours::mem {
    VmObject::VmObject(Type type, Flags vmof)
        : m_canary(type == Type::Paged ? kPagedMagic : kPhysicalMagic),
          m_type(type),
          m_flags(vmof),
          m_mappings(),
          m_children(),
          m_children_hook()
    { DEBUG_ASSERT(type == Type::Paged || type == Type::Physical); }
}