/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/vec.hpp>

#include <new>

#include <cassert>
#include <cstddef>

namespace {
/// The kernel allocator's contract on the host: |n| value-initialized
/// elements, destroyed again by deallocate().
template <typename T>
struct HeapAllocator {
    static auto allocate(std::size_t const n) -> T * {
        return new (std::nothrow) T[n]();
    }
    static auto deallocate(T *const p, std::size_t) -> void {
        delete[] p;
    }
};
} // namespace

auto main() -> int {
    ustl::collections::Vec<int, HeapAllocator<int>> values;

    // From nothing: one allocation of exactly the size asked, all zero.
    assert(values.resize(3).is_ok());
    assert(values.size() == 3U);
    assert(values.capacity() == 3U);
    assert(values[0] == 0 && values[2] == 0);

    // Growing keeps what is there.
    values[1] = 7;
    assert(values.resize(5).is_ok());
    assert(values.size() == 5U && values[1] == 7 && values[4] == 0);

    // Shrinking frees nothing, and growing back value-initializes the slots
    // again rather than showing what was cut off.
    values[3] = 9;
    assert(values.resize(2).is_ok());
    assert(values.size() == 2U && values.capacity() == 5U);
    assert(values.resize(5).is_ok());
    assert(values[1] == 7 && values[3] == 0);

    // Zero elements need no buffer.
    ustl::collections::Vec<int, HeapAllocator<int>> empty;
    assert(empty.resize(0).is_ok());
    assert(empty.data() == nullptr);
}
