/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PRIV_MEMORY_MODEL_HPP
#define OURS_MEM_PRIV_MEMORY_MODEL_HPP 1

#include <ours/mem/types.hpp>

#include <ours/init.hpp>

#include <ustl/result.hpp>

namespace ours::mem::priv {
    struct Altmap {
        PhysAddr start;
        PhysAddr end;
    };

    template <typename Derived>
    class MemoryModel {
        using Self = MemoryModel;
      public:
        auto init_nid(NnId nid, Pfn start, Pfn end, usize num_sections) -> Status;

      private:
        auto activate_section(NnId, Pfn start, usize nr_frames, Altmap *) -> ustl::Result<MemMap, Status>;
        auto deactivate_section(Pfn start, usize nr_frames, Altmap *) -> void;
    };

    class FlatModel: public MemoryModel<FlatModel> {
        using Self = FlatModel;
      public:
        MEM_INIT_CODE
        auto populate_memmap(Pfn start, usize nr_frames, NnId, Altmap *) -> ustl::Result<MemMap, Status>;

        MEM_INIT_CODE
        auto depopulate_memmap(Pfn start, usize nr_frames, NnId) -> Status;

        auto pfn_to_frame(Pfn) -> PmFrame *;
        auto frame_to_pfn(PmFrame *) -> Pfn;
    };

    class SparseModel: public MemoryModel<SparseModel> {
        using Self = SparseModel;
      public:
        MEM_INIT_CODE
        auto populate_memmap(Pfn start, usize nr_frames, NnId, Altmap *) -> ustl::Result<MemMap, Status>;

        MEM_INIT_CODE
        auto depopulate_memmap(Pfn start, usize nr_frames, NnId nid) -> Status;

        auto pfn_to_frame(Pfn) -> PmFrame *;
        auto frame_to_pfn(PmFrame *) -> Pfn;
    };

    class VflatModel: public MemoryModel<SparseModel> {
        using Self = VflatModel;
      public:
        MEM_INIT_CODE
        auto populate_memmap(Pfn start, usize nr_frames, NnId, Altmap *) -> ustl::Result<MemMap, Status>;

        MEM_INIT_CODE
        auto depopulate_memmap(Pfn start, usize nr_frames, NnId nid) -> Status;

        auto pfn_to_frame(Pfn) -> PmFrame *;
        auto frame_to_pfn(PmFrame *) -> Pfn;
    };
} // namespace ours::mem::priv

#endif // #ifndef OURS_MEM_PRIV_MEMORY_MODEL_HPP