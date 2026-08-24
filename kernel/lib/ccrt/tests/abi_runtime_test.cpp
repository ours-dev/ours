#include <ours/compiler.hpp>

using Guard = unsigned long long;
using CxaDestructor = auto (*)(void *) -> void;
using AtexitFunction = auto (*)() -> void;
using Initializer = auto (*)() -> void;

extern "C" Initializer const INITIALIZERS_START[] LINK_NAME("__ccrt_init_start") = {nullptr};
extern "C" Initializer const INITIALIZERS_END[] LINK_NAME("__ccrt_init_end") = {nullptr};

// These names are required by the compiler ABI and cannot follow local naming rules.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" {
extern void *__dso_handle;
auto __cxa_guard_acquire(Guard *) -> int;
auto __cxa_guard_release(Guard *) -> void;
auto __cxa_guard_abort(Guard *) -> void;
auto __cxa_atexit(CxaDestructor, void *, void *) -> int;
auto __cxa_finalize(void *) -> void;
auto atexit(AtexitFunction) noexcept -> int;
}
// NOLINTEND(readability-identifier-naming)

namespace {
int sequence;

struct StaticObject {
    ~StaticObject() {
        sequence = sequence * 10 + 4;
    }
};

auto static_object() -> StaticObject & {
    static StaticObject object;
    return object;
}

auto first(void *) -> void {
    sequence = sequence * 10 + 1;
}

auto second(void *) -> void {
    sequence = sequence * 10 + 2;
}

auto third() -> void {
    sequence = sequence * 10 + 3;
}
} // namespace

auto main() -> int {
    Guard initialized{};
    if (__cxa_guard_acquire(&initialized) != 1) {
        return 1;
    }
    __cxa_guard_release(&initialized);
    if (__cxa_guard_acquire(&initialized) != 0) {
        return 2;
    }

    Guard aborted{};
    if (__cxa_guard_acquire(&aborted) != 1) {
        return 3;
    }
    __cxa_guard_abort(&aborted);
    if (__cxa_guard_acquire(&aborted) != 1) {
        return 4;
    }
    __cxa_guard_release(&aborted);

    void *dso = &__dso_handle;
    sequence = 0;
    if (__cxa_atexit(first, nullptr, dso) != 0) {
        return 5;
    }
    if (__cxa_atexit(second, nullptr, dso) != 0) {
        return 6;
    }
    if (atexit(third) != 0) {
        return 7;
    }
    static_object();

    __cxa_finalize(dso);
    if (sequence != 421) {
        return 8;
    }

    __cxa_finalize(nullptr);
    if (sequence != 4213) {
        return 9;
    }

    __cxa_finalize(nullptr);
    if (sequence != 4213) {
        return 10;
    }
    return 0;
}
