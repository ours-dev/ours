/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PRIV_ALLOC_FAILURE_HPP
#define OURS_MM_PRIV_ALLOC_FAILURE_HPP

#if MM_TEST_ALLOC_FAILURE
namespace ours::mm::test {

enum class AllocationSite : u8 {
    /// Storage obtained through the kernel collection allocator.
    Collection,

    /// Storage for an object constructed by an object cache.
    Object,

    /// Storage requested through kernel new or new[].
    Heap,

    /// A frame for a page table.
    PageTable,

    /// A frame or block requested without an explicit node.
    Frame,
};

/// Fails one matching allocation on the constructing thread, after skipping
/// the requested number of matching allocations.
class AllocationFailure {
  public:
    explicit AllocationFailure(AllocationSite site, usize skip = 0);
    ~AllocationFailure();
    AllocationFailure(AllocationFailure const &) = delete;
    auto operator=(AllocationFailure const &) -> AllocationFailure & = delete;

    [[nodiscard]]
    auto triggered() const -> bool {
        return m_triggered;
    }

    [[nodiscard]]
    static auto hit(AllocationSite site) -> bool;

  private:
    AllocationSite const m_site;
    usize m_skip;
    bool m_triggered = false;
};

} // namespace ours::mm::test
#endif

#endif // OURS_MM_PRIV_ALLOC_FAILURE_HPP
