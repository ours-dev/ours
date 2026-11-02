/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HPP
#define OURS_OBJECT_HPP 1

// The object calls and the types they name, one domain in one file: a
// program that waits on objects, asks what they are, or walks their
// hierarchy includes this and nothing else of the system-call surface. Like
// zircon/syscalls/object.h, it gathers the calls that take any object; the
// per-topic record layouts live beside it (object_info.hpp) and the settable
// properties in object_property.hpp.

#include <ours/object_info.hpp>
#include <ours/object_property.hpp>
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
// The object calls, as a program calls them: generated from
// //system/public/ours/syscalls/object.gni (design 4.7), included inside
// namespace ours like the rest of the generated declarations.
#include <ours/vdso_decls/object.inc>
} // namespace ours

#endif // OURS_OBJECT_HPP
