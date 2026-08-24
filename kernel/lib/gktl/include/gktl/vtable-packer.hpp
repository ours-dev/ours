/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_VTABLE_PACKER_HPP
#define GKTL_VTABLE_PACKER_HPP 1

#include <ustl/mem/container_of.hpp>
#include <ustl/traits/invoke_args.hpp>
#include <ustl/util/move.hpp>
#include <ustl/util/type-list.hpp>

namespace gktl {
/// @since C++17
template <typename T, auto Member>
struct VtablePacker {
    template <auto Method>
    FORCE_INLINE constexpr static auto pack() {
        return []<typename... Args>(ustl::TypeList<Args...>) {
            // Marked FORCE_INLINE_ATTR to reduce meaningless function calls.
            //
            // In OS based C, the begin part of these kin of function in OPS design is
            // usually like the following.
            return [] FORCE_INLINE_ATTR(T * self, Args... args) {
                auto impl = ustl::mem::container_of(self, Member);
                return (impl->*Method)(ustl::forward<Args>(args)...);
            };
        }(typename ustl::traits::InvokeArgs<decltype(Method)>::Args());
    }
};
} // namespace gktl

#endif // #ifndef GKTL_VTABLE_PACKER_HPP
