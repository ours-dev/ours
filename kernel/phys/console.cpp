#include <ours/phys/console.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::phys {
namespace {
    using ConsoleList = ustl::collections::intrusive::List<Console, Console::ManagedOptions>;

    /// Constant-initialized, so it is ready before the first Console
    /// constructor runs — console instances are ordinary static objects
    /// brought to life by ccrt::init_global_static_objects().
    constinit ConsoleList g_consoles{};
} // namespace

Console::Console(char const *name) : m_name(name), m_hook() {
    g_consoles.push_back(*this);
}

auto init_early_console() -> void {
    for (auto &console: g_consoles) {
        console.activate();
    }
}

WEAK_LINK
auto arch_uart_handoff() -> UartHandoff {
    return {};
}

auto Console::write_all(char const *s, u32 n) -> void {
    for (auto &console: g_consoles) {
        console.write(s, n);
    }
}

auto vprint(ustl::views::StringView fmt) -> void {
    Console::write_all(fmt.data(), static_cast<u32>(fmt.size()));
}

} // namespace ours::phys
