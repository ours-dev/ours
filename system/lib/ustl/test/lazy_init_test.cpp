#include <ustl/lazy_init.hpp>

#include <cassert>
#include <cstdint>

namespace {
struct alignas(32) Value {
    explicit Value(std::uint32_t value) : value(value) {}

    std::uint32_t value;
};
} // namespace

auto main() -> int {
    ustl::LazyInit<Value> lazy;
    auto *value = lazy.init(42);

    assert(value == lazy.data());
    assert(value == &*lazy);
    assert(lazy->value == 42);
    assert(reinterpret_cast<std::uintptr_t>(value) % alignof(Value) == 0);
    return 0;
}
