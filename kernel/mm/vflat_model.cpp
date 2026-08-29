#include <ours/mm/priv/memory_model.hpp>

namespace ours::mm::priv {
auto VflatModel::populate_memmap(Pfn pfn, usize nr_frames, NnId nid, Altmap *altmap)
    -> ustl::Result<MemMap, Status> {
    return ustl::err(Status::NotSupported);
}

auto VflatModel::depopulate_memmap(Pfn pfn, usize nr_frames, NnId nid) -> Status {
    return Status::NotSupported;
}

} // namespace ours::mm::priv