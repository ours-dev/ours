/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the admission arithmetic: which floor a request gets in
/// each pass, and whether a zone's free count lets a block out above it. A
/// plain main() that returns non-zero on any failing check, as the other mm
/// smoke tests do.
///
/// The numbers are the default machine's Dma32 zone: Min 689, Low 861.
#include <ours/mm/priv/admission.hpp>

#include <cstdio>

namespace {
using ours::mm::admission_floor;
using ours::mm::AdmissionPass;
using ours::mm::admits;
using ours::mm::lowmem_reserve;

constexpr auto MIN = usize{689};
constexpr auto LOW = usize{861};

auto FAILURES = 0;

auto check(bool const ok, char const *const name) -> void {
    if (ok) {
        std::printf("admission-smoke: pass  %s\n", name);
        return;
    }
    FAILURES += 1;
    std::printf("admission-smoke: FAIL  %s\n", name);
}

auto floors() -> void {
    check(admission_floor(MIN, LOW, AdmissionPass::Low, true) == LOW &&
              admission_floor(MIN, LOW, AdmissionPass::Low, false) == LOW,
          "the first pass holds every request at Low");
    check(admission_floor(MIN, LOW, AdmissionPass::Min, true) == MIN,
          "the second pass lets a request that may block down to Min");
    check(admission_floor(MIN, LOW, AdmissionPass::Min, false) == MIN - MIN / 4,
          "the second pass lets a request that cannot block a quarter below Min");
    check(admission_floor(0, 0, AdmissionPass::Low, true) == 0 &&
              admission_floor(0, 0, AdmissionPass::Min, false) == 0,
          "with no watermarks set yet, nothing is held back");
}

auto counts() -> void {
    check(admits(LOW + 1, 0, LOW), "a frame out of Low + 1 leaves Low");
    check(!admits(LOW, 0, LOW), "a frame out of Low would leave Low - 1");
    check(admits(LOW + 8, 3, LOW) && !admits(LOW + 7, 3, LOW),
          "a block of eight counts as eight frames");
    check(!admits(3, 2, 0), "a block larger than the free count is refused even with no floor");
    check(admits(1, 0, 0), "with no floor the last frame goes");
}

/// The four rows of the worked example in the design: one zone, the free
/// count falling, the request an ordinary one that may block.
auto walk_down() -> void {
    auto const first = [](usize const free) {
        return admits(free, 0, admission_floor(MIN, LOW, AdmissionPass::Low, true));
    };
    auto const second = [](usize const free) {
        return admits(free, 0, admission_floor(MIN, LOW, AdmissionPass::Min, true));
    };
    check(first(900), "at 900 free the first pass admits");
    check(!first(800) && second(800), "at 800 the first pass refuses and the second admits");
    check(!first(MIN) && !second(MIN), "at Min both passes refuse a request that may block");
    check(admits(MIN, 0, admission_floor(MIN, LOW, AdmissionPass::Min, false)),
          "at Min a request that cannot block is still admitted");
}

/// The reserve on the default machine (Dma 3388 frames, Dma32 122515, no
/// Normal) and on a 3 GiB one (Normal 640150), by type index: Dma 0, Dma32 1,
/// Normal 2, and two types above that no node has here.
auto reserves() -> void {
    constexpr auto RATIO = usize{256};
    constexpr usize SMALL[] = {3388, 122515, 0, 0, 0};
    constexpr usize LARGE[] = {3388, 122515, 640150, 0, 0};
    constexpr auto COUNT = usize{5};

    check(lowmem_reserve(SMALL, COUNT, 0, 2, RATIO) == 478,
          "the default machine's Dma keeps 122515 / 256 frames from a request that allows Normal");
    check(lowmem_reserve(SMALL, COUNT, 1, 2, RATIO) == 0,
          "with no Normal zone, Dma32 keeps nothing back");
    check(lowmem_reserve(SMALL, COUNT, 0, 0, RATIO) == 0,
          "a request that allows nothing above Dma meets no reserve there");
    check(lowmem_reserve(LARGE, COUNT, 0, 2, RATIO) == (122515 + 640150) / 256 &&
              lowmem_reserve(LARGE, COUNT, 1, 2, RATIO) == 640150 / 256,
          "on 3 GiB both low zones keep a share of everything above them");
    check(lowmem_reserve(LARGE, COUNT, 0, 1, RATIO) == 122515 / 256,
          "a request that stops at Dma32 counts only Dma32 against Dma");
    check(lowmem_reserve(LARGE, COUNT, 2, 2, RATIO) == 0, "the highest zone keeps nothing back");

    // On the default machine a Dma zone at Low + reserve refuses a request
    // that allows Normal, and admits one that allows only Dma.
    constexpr auto DMA_LOW = usize{23};
    constexpr auto RESERVE = usize{478};
    check(!admits(DMA_LOW + RESERVE, 0, DMA_LOW + RESERVE) && admits(DMA_LOW + RESERVE, 0, DMA_LOW),
          "the reserve sits on top of the floor");
}

} // namespace

auto main() -> int {
    floors();
    counts();
    walk_down();
    reserves();

    if (FAILURES != 0) {
        std::printf("admission-smoke: FAILED -- %d check(s)\n", FAILURES);
        return 1;
    }
    std::printf("admission-smoke: ok\n");
    return 0;
}
