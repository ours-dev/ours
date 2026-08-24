/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_ITERATOR_REVERSE_ITERATOR_HPP
#define USTL_ITERATOR_REVERSE_ITERATOR_HPP 1

#include <iterator>

namespace ustl::iterator {

template <typename Iterator>
using RevIter = std::reverse_iterator<Iterator>;

} // namespace ustl::iterator

#endif // USTL_ITERATOR_REVERSE_ITERATOR_HPP
