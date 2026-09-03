#include <ours/mm/vm_object.hpp>

namespace ours::mm {
VmObject::VmObject(u32 magic, Type type, Flags vmof)
    : m_canary(magic), m_type(type), m_flags(vmof), m_mappings(), m_children_hook(), m_children() {
    DEBUG_ASSERT(type == Type::Paged || type == Type::Physical);
}
} // namespace ours::mm