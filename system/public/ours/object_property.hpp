/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_OBJECT_PROPERTY_HPP
#define OURS_OBJECT_PROPERTY_HPP 1

#include <ours/types.hpp>

namespace ours {
/// What ours::object_set_property writes. Each property names one value
/// layout and the object types that have it.
///
/// The values are Zircon's ZX_PROP_* (zircon/syscalls/object.h), as
/// InfoTopic's are ZX_INFO_*, so that Zircon's documentation of a property
/// describes the property with the same number here. Only ExceptionState is
/// answered; the others are added with the code that answers them.
enum class Property : u32 {
    /// On an exception handle: an ExceptionState (<ours/exception.hpp>), 4
    /// bytes. Zircon's ZX_PROP_EXCEPTION_STATE.
    ExceptionState = 16,
};
} // namespace ours

#endif // #ifndef OURS_OBJECT_PROPERTY_HPP
