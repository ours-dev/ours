/// Copyright 2026 The Ours Authors
///
/// Use of this source code is governed by a MIT-style
/// license that can be found in the LICENSE file or at
/// https://opensource.org/licenses/MIT

#ifndef OURS_PHYS_PHYSBOOT_HPP
#define OURS_PHYS_PHYSBOOT_HPP 1

#include <ours/compiler.hpp>
#include <ours/phys/physload.hpp>

namespace ours::phys {

/// physboot's module entry, declared to physload through PHYS_MODULE() in
/// physboot.cpp.  The name is physboot's own business -- physload finds it
/// through the module descriptor, never by symbol -- and this declaration
/// exists only so the definition and the PHYS_MODULE() reference to it can
/// sit in the same translation unit.
NO_RETURN
auto physboot_init(PhysLoadHandoff const &handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_PHYSBOOT_HPP
