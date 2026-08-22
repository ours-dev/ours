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

/// Each phys module (physboot, netboot, fastboot, ...) implements this
/// function.  physload-module calls it after installing the standard
/// services (console, allocation pool, symbolize) and running the
/// architecture handoff hook.
///
/// The implementation must be NO_RETURN — control never returns to
/// physload-module.
NO_RETURN
auto phys_load_module_main(PhysLoadHandoff const &handoff) -> void;

} // namespace ours::phys

#endif // OURS_PHYS_PHYSBOOT_HPP
