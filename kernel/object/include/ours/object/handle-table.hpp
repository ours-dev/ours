/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_HANDLE_TABLE_HPP
#define OURS_OBJECT_HANDLE_TABLE_HPP 1

#include <ours/types.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/dispatcher.hpp>

#include <ustl/rc.hpp>
#include <ustl/sync/atomic.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::object {
    class HandleTable: public ustl::RefCounter<HandleTable> {
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

        CXX11_CONSTEXPR
        static auto const kNumHandlesPerBlock = 8;

        using HandleList = ustl::collections::Deque<
            Handle, 
            Allocator<Handle>,
            ustl::collections::DequeOptions<
                ustl::collections::BlockSize<kNumHandlesPerBlock>
            >
        >;

        KoId m_koid;
        HandleList m_handles;
    };
} // namespace ours::object

#endif // #ifndef OURS_OBJECT_HANDLE_TABLE_HPP