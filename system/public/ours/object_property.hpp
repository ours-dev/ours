/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_OBJECT_PROPERTY_HPP
#define OURS_OBJECT_PROPERTY_HPP 1

#include <ours/types.hpp>

namespace ours {
/// What ours::object_set_property writes. Each property names one value
/// layout and the object types that have it.
///
/// The values are ABI and never change, as InfoTopic's. Only ExceptionState
/// is answered; the others are added with the code that answers them.
enum class Property : u32 {
    /// On an exception handle: an ExceptionState (<ours/exception.hpp>), 4
    /// bytes.
    ExceptionState = 16,
};
} // namespace ours

#endif // #ifndef OURS_OBJECT_PROPERTY_HPP
