/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_ITERATOR_TRAITS_HPP
#define USTL_ITERATOR_TRAITS_HPP 1

#include <iterator>
#include <type_traits>

namespace ustl::iterator {

template <typename Iterator>
struct IterTraits : std::iterator_traits<Iterator> {
    using Element = typename std::iterator_traits<Iterator>::value_type;
    using Distance = typename std::iterator_traits<Iterator>::difference_type;
    using Category = typename std::iterator_traits<Iterator>::iterator_category;
    using Pointer = typename std::iterator_traits<Iterator>::pointer;
    using Reference = typename std::iterator_traits<Iterator>::reference;
};

template <typename T>
struct IterTraits<T *> {
    using Element = std::remove_cv_t<T>;
    using Distance = std::ptrdiff_t;
    using Category = std::random_access_iterator_tag;
    using Pointer = T *;
    using Reference = T &;
};

template <typename Iterator>
using CategoryT = typename IterTraits<std::remove_cvref_t<Iterator>>::Category;

template <typename Iterator>
using DistanceOfT = typename IterTraits<std::remove_cvref_t<Iterator>>::Distance;

template <typename Iterator>
using ElementOfT = typename IterTraits<std::remove_cvref_t<Iterator>>::Element;

} // namespace ustl::iterator

#endif // USTL_ITERATOR_TRAITS_HPP
