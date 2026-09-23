/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP
#define OURS_OBJECT_PAGER_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>

namespace ours::object {
class PagerDispatcher;

/// Zircon ZX_DEFAULT_PAGER_RIGHTS.
constexpr auto RIGHTS_DEFAULT_PAGER =
    Rights::Inspect | Rights::Transfer | Rights::AttachVmo | Rights::ManageVmo;

using PagerDispatcherBase = SoloDispatcher<PagerDispatcher, RIGHTS_DEFAULT_PAGER>;

class PagerDispatcher: public PagerDispatcherBase {
    using Self = PagerDispatcher;
    using Base = PagerDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Pager);

    enum class Operation : u8 {
        Dirty,
        WriteBackPreamble,
        WriteBackPostamble,
    };

    auto operate_range(Operation op, ustl::Arc<mm::VmObject> vmo, usize offset, usize len,
                       usize priv_data) -> Status;

  private:
    static constexpr auto MAGIC = gktl::magic("PagerDispatcher");
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP