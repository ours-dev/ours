#include <ours/panic.hpp>
#include <ours/phys/print.hpp>

namespace ours {
[[noreturn]]
auto panic() -> void {
    phys::vprint("panic");
    while (1)
        ;
}

[[noreturn]]
auto vpanic(std::string_view fmt) -> void {
    phys::vprint("panic: ");
    phys::vprint(fmt);
    phys::vprint("\n");
    while (1)
        ;
}
} // namespace ours
