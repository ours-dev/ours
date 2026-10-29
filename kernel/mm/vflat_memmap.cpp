/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/priv/vflat_memmap.hpp>

namespace ours::mm::priv {
/// Not written yet. Backing the window means building page tables at a point in
/// boot where VmAspace::kernel_aspace() does not exist, so this needs the same
/// treatment CpuLocal::Mapper gets -- a thin ArchVmAspace over the kernel PGD
/// that is already installed.
auto VflatMemMap::populate_memmap(Pfn pfn, usize nr_frames, NodeId nid, Altmap *altmap)
    -> ustl::Result<FrameSpan, Status> {
    return ustl::err(Status::NotSupported);
}

auto VflatMemMap::depopulate_memmap(Pfn pfn, usize nr_frames, NodeId nid) -> Status {
    return Status::NotSupported;
}

} // namespace ours::mm::priv
