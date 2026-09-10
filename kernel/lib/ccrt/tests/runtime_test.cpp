#include <ours/compiler.hpp>

#include <ccrt/runtime.hpp>

extern "C" ccrt::Initializer const INITIALIZERS_START[] LINK_NAME("__ccrt_init_start") = {nullptr};
extern "C" ccrt::Initializer const INITIALIZERS_END[] LINK_NAME("__ccrt_init_end") = {nullptr};

namespace {
int SEQUENCE;

auto first(void *) -> void {
    SEQUENCE = SEQUENCE * 10 + 1;
}

auto second(void *) -> void {
    SEQUENCE = SEQUENCE * 10 + 2;
}

auto third() -> void {
    SEQUENCE = SEQUENCE * 10 + 3;
}

auto fourth(void *) -> void {
    SEQUENCE = SEQUENCE * 10 + 4;
}
} // namespace

auto main() -> int {
    ccrt::Guard initialized{};
    if (!initialized.acquire()) {
        return 1;
    }
    initialized.release();
    if (initialized.acquire()) {
        return 2;
    }

    ccrt::Guard aborted{};
    if (!aborted.acquire()) {
        return 3;
    }
    aborted.abort();
    if (!aborted.acquire()) {
        return 4;
    }
    aborted.release();

    int scope_token;
    auto const scope = ccrt::Scope{&scope_token};
    auto &registry = ccrt::Registry::instance();
    SEQUENCE = 0;
    if (!registry.register_destructor(first, nullptr, scope)) {
        return 5;
    }
    if (!registry.register_destructor(second, nullptr, scope)) {
        return 6;
    }
    if (!registry.register_exit_function(third)) {
        return 7;
    }

    registry.finalize(scope);
    if (SEQUENCE != 21) {
        return 8;
    }
    registry.finalize();
    if (SEQUENCE != 213) {
        return 9;
    }
    registry.finalize();
    if (SEQUENCE != 213) {
        return 10;
    }

    if (!registry.register_destructor(fourth, nullptr, scope)) {
        return 11;
    }
    registry.finalize(scope);
    if (SEQUENCE != 2134) {
        return 12;
    }
    return 0;
}
