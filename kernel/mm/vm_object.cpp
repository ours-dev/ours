#include <ours/mm/vm_object.hpp>

namespace ours::mm {
    VmObject::VmObject(Type type, Flags vmof)
        : m_canary(static_cast<u32>(type)),
          m_type(type),
          m_flags(vmof),
          m_mappings(),
          m_children_hook(),
          m_children()
    { DEBUG_ASSERT(type == Type::Paged || type == Type::Physical); }
}