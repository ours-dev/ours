/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_POINTER_REBIND_HPP
#define USTL_COLLECTIONS_INTRUSIVE_POINTER_REBIND_HPP 1

#include <memory>

namespace ustl::collections::intrusive {
template <typename PtrMut, typename T>
struct PointerRebind final {
    using Type = typename std::pointer_traits<PtrMut>::template rebind<T>;
};

template <typename PtrMut, typename T>
    requires requires { typename PtrMut::template Rebind<T>; }
struct PointerRebind<PtrMut, T> final {
    using Type = typename PtrMut::template Rebind<T>;
};

template <typename T, typename U>
struct PointerRebind<T *, U> final {
    using Type = U *;
};
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_POINTER_REBIND_HPP
