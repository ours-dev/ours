/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_PTR_HPP
#define USTL_TRAITS_PTR_HPP 1

#include <type_traits>

namespace ustl::traits {

template <typename T>
using IsPtr = std::is_pointer<T>;

template <typename T>
inline constexpr bool IsPtrV = std::is_pointer_v<T>;

template <typename T>
using AddPtr = std::add_pointer<T>;

template <typename T>
using AddPtrT = std::add_pointer_t<T>;

template <typename T>
using RemovePtr = std::remove_pointer<T>;

template <typename T>
using RemovePtrT = std::remove_pointer_t<T>;

template <typename T>
struct PtrDepth: std::integral_constant<int, 0> {};

template <typename T>
struct PtrDepth<T *>: std::integral_constant<int, PtrDepth<T>::value + 1> {};

template <typename T>
inline constexpr int PtrDepthV = PtrDepth<T>::value;

template <typename T, int Depth>
struct AddPtrDepth {
    using Type = AddPtrT<typename AddPtrDepth<T, Depth - 1>::Type>;
};

template <typename T>
struct AddPtrDepth<T, 0> {
    using Type = T;
};

template <typename T, int Depth>
using AddPtrDepthT = typename AddPtrDepth<T, Depth>::Type;

template <typename T, int Depth>
struct RemovePtrDepth {
    using Type = RemovePtrT<typename RemovePtrDepth<T, Depth - 1>::Type>;
};

template <typename T>
struct RemovePtrDepth<T, 0> {
    using Type = T;
};

template <typename T, int Depth>
using RemovePtrDepthT = typename RemovePtrDepth<T, Depth>::Type;

} // namespace ustl::traits

#endif // USTL_TRAITS_PTR_HPP
