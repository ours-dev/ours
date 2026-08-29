/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_DETAIL_DEALLOCATE_HPP
#define USTL_DETAIL_DEALLOCATE_HPP 1

#include <type_traits>

namespace ustl::detail {

/// Detects a nested `Disposer` customization point on T. When present, it is
/// used as the storage-reclaim strategy for the owning smart pointers
/// (`RefCounted<T>`, `Box<T>`); otherwise the plain `delete` operator is used.
template <typename T, typename = void>
struct HasDisposer: std::false_type {};
template <typename T>
struct HasDisposer<T, std::void_t<typename T::Disposer>>: std::true_type {};

/// Default storage reclaim: a nested `T::Disposer` if present, otherwise
/// plain `operator delete` (in the kernel this is `ktl::kfree`). The contract
/// is reclaim-only: the payload destructor has already run when this is
/// called, so a Disposer must never destroy the object itself.
template <typename T>
auto default_deallocate(void *pointer) noexcept -> void {
    if constexpr (HasDisposer<T>::value) {
        typename T::Disposer{}(static_cast<T *>(pointer));
    } else {
        ::operator delete(pointer);
    }
}

} // namespace ustl::detail

#endif // USTL_DETAIL_DEALLOCATE_HPP
