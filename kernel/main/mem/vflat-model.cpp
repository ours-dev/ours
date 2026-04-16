#include <ours/mem/priv/memory-model.hpp>

namespace ours::mem::priv {
    auto VflatModel::populate_memmap(Pfn pfn, usize nr_frames, NnId nid) 
        -> ustl::Result<MemMap, Status> {
        
    }

    auto VflatModel::depopulate_memmap(Pfn pfn, usize nr_frames, NnId nid) -> Status {

    }

} // namespace ours::mem::priv