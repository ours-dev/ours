/// Command pattern with CRTP auto-registration.
///
/// Each concrete command inherits from AutoCommand<Self> and is automatically
/// registered with the CommandRegistry singleton — no manual wiring needed.
/// New commands only need a .cpp file; nothing else changes.

#ifndef OBI_CLI_COMMAND_HPP
#define OBI_CLI_COMMAND_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace obit {

/// Abstract command — named, self-describing handler.
class Command {
  public:
    Command(std::string name, std::string description, std::string usage_args)
        : name_(std::move(name))
        , description_(std::move(description))
        , usage_args_(std::move(usage_args)) {}

    virtual ~Command() = default;

    [[nodiscard]]
    auto name() const -> std::string const & {
        return name_;
    }
    [[nodiscard]]
    auto description() const -> std::string const & {
        return description_;
    }
    [[nodiscard]]
    auto usage_args() const -> std::string const & {
        return usage_args_;
    }

    [[nodiscard]]
    virtual auto execute(std::vector<std::string> const &args) const -> int = 0;

  private:
    std::string name_;
    std::string description_;
    std::string usage_args_;
};

/// Singleton registry — collects commands registered at static-init time.
class CommandRegistry final {
  public:
    static auto instance() -> CommandRegistry & {
        static CommandRegistry registry;
        return registry;
    }

    auto add(std::unique_ptr<Command> cmd) -> void {
        auto const name = cmd->name();
        commands_.emplace(name, std::move(cmd));
    }

    [[nodiscard]]
    auto find(std::string_view const name) const -> Command const * {
        auto const it = commands_.find(name);
        return (it != commands_.end()) ? it->second.get() : nullptr;
    }

    [[nodiscard]]
    auto all() const -> std::map<std::string, std::unique_ptr<Command>, std::less<>> const & {
        return commands_;
    }

  private:
    CommandRegistry() = default;

    std::map<std::string, std::unique_ptr<Command>, std::less<>> commands_;
};

/// RAII registrar — created at static-init time to self-register a command.
struct CommandRegistrar {
    explicit CommandRegistrar(std::unique_ptr<Command> cmd) {
        CommandRegistry::instance().add(std::move(cmd));
    }
};

/// CRTP base: inherit from AutoCommand<Self>, and Self is auto-registered.
/// Usage:
///   class ValidateCommand final : public AutoCommand<ValidateCommand> { ... };
/// No manual registration call needed — the static member takes care of it.
template <typename Derived>
class AutoCommand: public Command {
  protected:
    AutoCommand(std::string name, std::string description, std::string usage_args)
        : Command(std::move(name), std::move(description), std::move(usage_args)) {
        // Touch registrar_ to ensure the static is instantiated.
        (void)&registrar_;
    }

  private:
    inline static CommandRegistrar registrar_{std::make_unique<Derived>()};
};

/// Builds a synthetic argv (program name + args) for cxxopts::Options::parse,
/// which takes `const char* const*` and does not write through the pointers
/// (cxxopts v3 copies internally), so borrowing c_str() pointers from `args`
/// for the lifetime of the parse() call is safe.
inline auto make_argv(std::string const &program, std::vector<std::string> const &args)
    -> std::vector<char const *> {
    std::vector<char const *> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(program.c_str());
    for (auto const &arg: args) {
        argv.push_back(arg.c_str());
    }
    return argv;
}

} // namespace obit

#endif
