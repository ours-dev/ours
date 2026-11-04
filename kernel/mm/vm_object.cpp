#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object.hpp>

namespace ours::mm {
VmObject::VmObject(u32 magic, Type type, Flags vmof, sync::Mutex &lock)
    : m_canary(magic)
    , m_type(type)
    , m_flags(vmof)
    , m_lock(lock)
    , m_mappings()
    , m_children_hook()
    , m_children() {
    DEBUG_ASSERT(type == Type::Paged || type == Type::Physical);
}

auto VmObject::operate_range_locked(usize const offset, usize const len, RangeOp const op) -> void {
    for (auto &mapping: m_mappings) {
        mapping.operate_range_locked_object(offset, len, op);
    }
}
} // namespace ours::mm