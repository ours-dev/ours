/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_PAGING_MAPPING_MAPPING_HPP
#define ARCH_PAGING_MAPPING_MAPPING_HPP 1

#include <arch/paging/mmu_flags.hpp>
#include <arch/types.hpp>
#include <ours/compiler.hpp>
namespace arch::paging {
struct MappingAddress {
    PhysAddr phys;
    VirtAddr virt;
};

///
struct VirtAddrCursor {
    FORCE_INLINE
    constexpr VirtAddrCursor() = default;

    FORCE_INLINE
    constexpr VirtAddrCursor(VirtAddr virt, usize size)
        : size_(size), consumed_(0), addr_(virt) {}

    FORCE_INLINE
    constexpr auto size() const -> usize {
        return size_;
    }

    FORCE_INLINE
    constexpr auto virt_addr() const -> VirtAddr {
        return addr_ + consumed_;
    }

    FORCE_INLINE
    constexpr auto consume(usize page_size) -> void {
        consumed_ += page_size;
    }

    FORCE_INLINE
    constexpr auto remaining_size() const -> usize {
        return size_ - consumed_;
    }

    FORCE_INLINE
    constexpr auto consumed_range() const -> VirtAddrCursor {
        return VirtAddrCursor(addr_, consumed_);
    }

    usize size_;
    usize consumed_;
    VirtAddr addr_;
};

struct PhysAddrCursor {
    FORCE_INLINE
    constexpr PhysAddrCursor() = default;

    FORCE_INLINE
    constexpr PhysAddrCursor(PhysAddr *phys, usize n, usize page_size)
        : page_size_(page_size), consumed_(0), count_(n), addrs_(phys) {}

    FORCE_INLINE
    constexpr auto phys_addr() const -> PhysAddr {
        return (*addrs_) + consumed_;
    }

    FORCE_INLINE
    constexpr auto consume(usize page_size) -> void {
        consumed_ += page_size;
        if (consumed_ == page_size_) {
            consumed_ = 0;
            count_ -= 1;
            addrs_ += 1;
        }
    }

    constexpr auto remaining_size() const -> usize {
        if (!count_) {
            return 0;
        }
        return page_size_ - consumed_;
    }

    usize page_size_;
    usize consumed_;
    usize count_;
    PhysAddr *addrs_;
};

/// Walk state carried across page-table levels. `ArchFlags` is the paging
/// scheme's own PTE flag type (e.g. X86MmuFlags), supplied by the caller —
/// `Paging<ArchPaging>` binds it from `ArchPaging::ArchMmuFlags`. Neutral
/// MmuFlags are converted once, on construction, via MmuFlagsConv<ArchFlags>.
template <typename ArchFlags>
struct BasicTravelContext {
    typedef BasicTravelContext Self;

    BasicTravelContext() = default;

    BasicTravelContext(VirtAddrCursor cursor, MmuFlags mmuf = {})
        : num_travelled_(0), flags_(MmuFlagsConv<ArchFlags>::to_arch(mmuf)), virt_cursor_(cursor) {}

    BasicTravelContext(VirtAddr va, usize nr_pages, usize page_size, MmuFlags mmuf = {})
        : Self(VirtAddrCursor(va, nr_pages * page_size), mmuf) {}

    FORCE_INLINE
    constexpr auto consume(usize page_size) -> void {
        virt_cursor_.consume(page_size);
        num_travelled_ += 1;
    }

    FORCE_INLINE
    constexpr auto size() const -> usize {
        return virt_cursor_.size();
    }

    FORCE_INLINE
    constexpr auto mmuflags() const -> ArchFlags {
        return flags_;
    }

    FORCE_INLINE
    constexpr auto num_travelled() const -> usize {
        return num_travelled_;
    }

    FORCE_INLINE
    auto virt_cursor() -> VirtAddrCursor & {
        return virt_cursor_;
    }

    FORCE_INLINE
    constexpr auto virt_addr() const -> VirtAddr {
        return virt_cursor_.virt_addr();
    }

    FORCE_INLINE
    auto set_cursor(VirtAddrCursor cursor) -> Self & {
        virt_cursor_ = cursor;
        return *this;
    }

  protected:
    usize num_travelled_;
    ArchFlags flags_;
    VirtAddrCursor virt_cursor_;
};

template <typename ArchFlags>
struct BasicMapContext: public BasicTravelContext<ArchFlags> {
    typedef BasicMapContext Self;
    typedef BasicTravelContext<ArchFlags> Base;

    using Base::num_travelled_;
    using Base::virt_cursor_;

    BasicMapContext() = default;

    FORCE_INLINE
    constexpr BasicMapContext(VirtAddr va, PhysAddr *pa, usize n, usize page_size, MmuFlags flags)
        : Base(va, n, page_size, flags), phys_cursor_(pa, n, page_size) {}

    FORCE_INLINE
    constexpr auto phys_addr() const -> PhysAddr {
        return phys_cursor_.phys_addr();
    }

    FORCE_INLINE
    auto phys_cursor() -> PhysAddrCursor & {
        return phys_cursor_;
    }

    FORCE_INLINE
    constexpr auto num_mapped() const -> usize {
        return num_travelled_;
    }

    FORCE_INLINE
    auto consume(usize page_size) -> void {
        Base::consume(page_size);
        phys_cursor_.consume(page_size);
    }

    FORCE_INLINE
    auto skip(usize page_size) -> void {
        virt_cursor_.consume(page_size);
    }

    FORCE_INLINE
    auto take(usize page_size) -> MappingAddress {
        auto const phys = phys_cursor_.phys_addr();
        auto const virt = virt_cursor_.virt_addr();
        this->consume(page_size);
        return {phys, virt};
    }

    FORCE_INLINE
    auto has_more() const -> bool {
        return phys_cursor_.count_ > 0;
    }

  protected:
    PhysAddrCursor phys_cursor_;
};

} // namespace arch::paging

#endif // #ifndef ARCH_PAGING_MAPPING_MAPPING_HPP
