/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_CONSOLE_HPP
#define OURS_PHYS_CONSOLE_HPP 1

#include <ours/compiler.hpp>
#include <ours/types.hpp>
#include <ustl/collections/intrusive/list_hook.hpp>

namespace ours::phys {

/// Early-boot console with link-time auto registration.
///
/// A console implementation registers itself by declaring a file-scope
/// static instance in its own translation unit:
///
///     namespace {
///     SerialConsole g_serial_console;   // constructor runs via .ccrt_init
///     }
///
/// There is no central list and no registration macro.  The instance's
/// constructor calls Console(char const*) below, which links the object
/// into a constinit intrusive list — constant-initialized, so there is no
/// dynamic-initialization ordering hazard between the list and any
/// console.  Constructors only run when the image entry point calls
/// activate(), which init_early_console() (ours/phys/init.hpp) invokes on
/// every registered console before the first print.
class PHYS_EXPORT Console {
    using Self = Console;

  public:
    Console(Console const &) = delete;
    auto operator=(Console const &) = delete;

    /// Write to every registered console.  Consoles that have not been
    /// activated yet still receive the bytes via their write() defaults.
    static auto write_all(char const *s, u32 n) -> void;

    virtual auto write(char const *s, u32 n) -> void = 0;
    virtual auto read(char *s, u32 n) -> u32 = 0;

    virtual auto activate() -> void {}
    virtual auto deactivate() -> void {}

    [[nodiscard]]
    auto name() const -> char const * {
        return m_name;
    }

  protected:
    /// Self-registers the console into the global list.
    Console(char const *name);

    virtual ~Console() = default;

  private:
    char const *m_name;

    /// Intrusive-list link plus its option tag; publicized below so the
    /// constinit global list can live at file scope in console.cpp.
    ustl::collections::intrusive::ListMemberHook<> m_hook;

  public:
    USTL_DECLARE_HOOK_OPTION(Console, m_hook, ManagedOptions);
};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_CONSOLE_HPP
