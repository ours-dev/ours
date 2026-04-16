/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_VM_AREA_DISPATCHER_HPP
#define OURS_OBJECT_VM_AREA_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>

namespace ours::object {
    using VmAreaDispatcherBase = SoloDispatcher<VmAreaDispatcher,
                                                kDefaultRights,
                                                Signals::None>;

    class VmAreaDispatcher: public VmAreaDispatcherBase {};

} // namespace ours::object

#endif // #ifndef OURS_OBJECT_VM_AREA_DISPATCHER_HPP