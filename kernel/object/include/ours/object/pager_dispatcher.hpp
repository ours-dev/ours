/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP
#define OURS_OBJECT_PAGER_DISPATCHER_HPP 1

#include <ours/object/dispatcher.hpp>

namespace ours::object {
    class PagerDispatcher;

    using PagerDispatcherBase = SoloDispatcher<PagerDispatcher, Rights::Default>;

    class PagerDispatcher: public PagerDispatcherBase {
        using Self = PagerDispatcher;
        using Base = PagerDispatcherBase;
      public:
        enum class Operation: u8 {
            Dirty,
            WriteBackPreamble,
            WriteBackPostamble,
        };

        auto operate_range(Operation op, ustl::Arc<mm::VmObject> vmo, usize offset, usize len, usize priv_data) -> Status;
      private:
        static constexpr auto MAGIC = gktl::magic("PagerDispatcher");
    };
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_PAGER_DISPATCHER_HPP