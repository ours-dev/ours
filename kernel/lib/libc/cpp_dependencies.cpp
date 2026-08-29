#include <ours/panic.hpp>

#include <exception>
#include <functional>
#include <stdexcept>
#include <version>

extern "C" {
auto __cdecl _purecall() -> void {}
auto __cxa_pure_virtual() -> void {}
}

#include <new>

[[gnu::weak]]
auto operator new(std::size_t) -> void * {
    ours::panic(__func__);
}

[[gnu::weak]]
auto operator new(std::size_t, std::align_val_t) -> void * {
    ours::panic(__func__);
}

[[gnu::weak]]
auto operator delete(void *) -> void {}

[[gnu::weak]]
auto operator delete(void *, std::size_t) -> void {}

[[gnu::weak]]
auto operator delete(void *, std::size_t, std::align_val_t) -> void {}
