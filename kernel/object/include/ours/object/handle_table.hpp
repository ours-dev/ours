/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_TABLE_HPP
#define OURS_OBJECT_HANDLE_TABLE_HPP 1

#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/types.hpp>

#include <ustl/rc.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
class HandleTable: public ustl::RefCounted<HandleTable, ustl::sync::ThreadSafePolicy> {
    using Self = HandleTable;

  public:
    HandleTable();
    ~HandleTable();

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(HandleTable);

    template <typename T>
    class Allocator {
        using Self = Allocator;

      public:
        using value_type = T;

        template <typename U>
        using rebind = Allocator<U>;

        auto allocate() -> Handle *;
    };

    constexpr static auto NUM_HANDLES_PER_BLOCK = 8;

    // TODO(SmallHuaZi): the handle storage is still to be designed. It used to
    // be declared as
    //
    //     using HandleList = ustl::collections::Deque<
    //         Handle, Allocator<Handle>,
    //         DequeOptions<BlockSize<NUM_HANDLES_PER_BLOCK>>>;
    //     HandleList m_handles;
    //
    // over Boost.Container, which this project does not depend on. That
    // declaration never typechecked anyway: the Allocator above exposes a
    // no-argument `allocate() -> Handle *`, which satisfies no container's
    // allocator contract. Nothing reads m_handles yet -- HandleTable has no
    // implementation file and no callers -- so the member is parked here
    // rather than guessed at, together with the block-allocated intent that
    // NUM_HANDLES_PER_BLOCK records.

    KoId m_koid;
};
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_HANDLE_TABLE_HPP