#include <cstdlib>

#include <ours/cpu_mask.hpp>

namespace {
auto require(bool condition) -> void {
    if (!condition) {
        std::abort();
    }
}
} // namespace

auto main() -> int {
    auto mask = ours::CpuMask::of(0);
    mask.set(7).set(15).set(16);
    require(mask.count() == 3);
    require(mask.test(0) && mask.test(7) && mask.test(15));
    require(!mask.test(16));

    auto selected = ours::CpuMask::of(7) | ours::CpuMask::of(9);
    auto const intersection = mask & selected;
    require(intersection.count() == 1 && intersection.test(7));
    require(!intersection.test(0));

    auto visited = usize{0};
    mask.for_each([&visited](ours::CpuNum) { ++visited; });
    require(visited == mask.count());
    mask.set(7, false);
    require(mask.count() == 2 && !mask.test(7));
    require(mask.any());
    mask.reset();
    require(mask.none());
    return 0;
}
