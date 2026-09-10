#include <gktl/init_hook.hpp>

#include <algorithm>
#include <span>

namespace gktl {
namespace {
    extern "C" InitHook INIT_HOOK_START[] LINK_NAME("__init_hook_start");
    extern "C" InitHook INIT_HOOK_END[] LINK_NAME("__init_hook_end");

    std::span<InitHook> INIT_HOOKS{INIT_HOOK_START, INIT_HOOK_END};
} // namespace

InitLevel INIT_LEVEL = InitLevel::LowestLevel;

/// The hooks in range [level, level + delta) would be called.
auto set_init_level(InitLevel level, i32 delta) -> void {
    if (INIT_LEVEL == InitLevel::LowestLevel) {
        std::ranges::sort(INIT_HOOKS, [](InitHook const &lhs, InitHook const &rhs) -> bool {
            return lhs.level < rhs.level;
        });
    }

    auto it = INIT_HOOKS.begin();
    for (auto end = INIT_HOOKS.end(); it != end; ++it) {
        if (it->level >= level + delta) {
            break;
        }

        it->fn();
    }

    INIT_HOOKS = std::span<InitHook>(it, INIT_HOOKS.end());
    INIT_LEVEL = std::max(INIT_LEVEL, level);
}

} // namespace gktl
