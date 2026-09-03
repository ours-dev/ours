#include <logz4/log.hpp>
#include <ours/phys/console.hpp>
#include <ours/phys/init.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours::phys {
namespace {
    using ConsoleList = ustl::collections::intrusive::List<Console, Console::ManagedOptions>;

    /// Constant-initialized, so it is ready before the first Console
    /// constructor runs — console instances are ordinary static objects
    /// brought to life by ccrt::init_global_static_objects().
    constinit ConsoleList CONSOLES{};

    /// Bridges logz4's rendered lines to every registered console. The
    /// phys image installs this as the global logger in
    /// init_early_console(), so the whole boot tree logs through logz4.
    class ConsoleLogger final: public logz4::Logger {
      public:
        auto write(char const *s, usize n) -> void override {
            Console::write_all(s, static_cast<u32>(n));
        }
    };

    ConsoleLogger CONSOLE_LOGGER;
} // namespace

Console::Console(char const *name) : m_name(name), m_hook() {
    CONSOLES.push_back(*this);
}

auto init_early_console() -> void {
    for (auto &console: CONSOLES) {
        console.activate();
    }
    // From here on, the whole phys tree logs through logz4.
    logz4::set_global_logger(CONSOLE_LOGGER);
}

WEAK_LINK auto arch_uart_handoff() -> UartHandoff {
    return {};
}

auto Console::write_all(char const *s, u32 n) -> void {
    for (auto &console: CONSOLES) {
        console.write(s, n);
    }
}

} // namespace ours::phys
