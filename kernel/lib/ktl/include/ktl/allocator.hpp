/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_ALLOCATOR_HPP
#define KTL_ALLOCATOR_HPP 1

#include <ktl/new.hpp>

namespace ktl {
template <typename T>
struct Allocator {
    typedef T Element;
    typedef Element *PtrMut;
    typedef Element const *Ptr;
    typedef Element &RefMut;
    typedef Element const &Ref;

    typedef Element value_type;
    typedef PtrMut pointer;
    typedef Ptr const_pointer;
    typedef RefMut reference;
    typedef Ref const_reference;

    template <typename U>
    using RebindT = Allocator<U>;

    static auto allocate(usize n) -> PtrMut {
        return new (ours::mm::GAF_KERNEL) Element[n]();
    }

    static auto deallocate(Ptr ptr, usize n) -> void {
        delete[] ptr;
    }
};

} // namespace ktl

#endif // #ifndef KTL_ALLOCATOR_HPP
