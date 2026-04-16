/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_CONSOLE_HPP
#define OURS_PHYS_CONSOLE_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>

#include <ustl/collections/intrusive/list_hook.hpp>

#define REGISTER_CONSOLE(TYPE, NAME, ...) \
    [[gnu::init_priority(1000)]] \
    static TYPE NAME{__VA_ARGS__};

namespace ours::phys {
    struct Console {
        Console(char const *name);
        virtual ~Console() = default;

        virtual auto write(char const *s, u32 n) -> void = 0;
        virtual auto read(char *s, u32 n) -> u32 = 0;

        virtual auto activate() -> void {}
        virtual auto deactivate() -> void {}

        char const *name_;
        ustl::collections::intrusive::ListMemberHook<> hook_;
        USTL_DECLARE_HOOK_OPTION(Console, hook_, ManagedOptions);
    };
} // namespace ours::phys

#endif // #ifndef OURS_PHYS_CONSOLE_HPP