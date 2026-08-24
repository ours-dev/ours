#include <ustl/sync/atomic.hpp>

#include <cassert>

auto main() -> int {
    int storage = 3;
    ustl::sync::AtomicRef<int> value(storage);
    value.store(7, ustl::sync::MemoryOrder::Relaxed);
    assert(storage == 7);
    assert(value.load(ustl::sync::MemoryOrder::Relaxed) == 7);
    assert(value.fetch_add(5, ustl::sync::MemoryOrder::Relaxed) == 7);
    assert(storage == 12);
    return 0;
}
