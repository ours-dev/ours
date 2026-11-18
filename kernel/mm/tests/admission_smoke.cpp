/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Host unit test for the admission arithmetic: which floor a request gets in
/// each pass, and whether a zone's free count lets a block out above it. A
/// plain main() that runs every check and returns non-zero if any failed
/// (SmokeChecks), as alloc_request_smoke does.
///
/// The numbers are the default machine's Dma32 zone: Min 689, Low 861.
#include <ours/mm/priv/admission.hpp>

#include "smoke_checks.hpp"

namespace {
using ours::mm::AdmissionPass;
using ours::mm::admits;
using ours::mm::lowmem_reserve;
using ours::mm::Watermarks;
using ours::mm::test::SmokeChecks;

constexpr auto MIN = usize{689};
constexpr auto LOW = usize{861};

/// The zone the numbers are from. High is Low plus the band again.
constexpr auto ZONE = Watermarks{.min = MIN, .low = LOW, .high = LOW + (LOW - MIN)};

auto floors(SmokeChecks &checks) -> void {
    checks.check(ZONE.floor(AdmissionPass::Low, true) == LOW &&
                     ZONE.floor(AdmissionPass::Low, false) == LOW,
                 "the first pass holds every request at Low");
    checks.check(ZONE.floor(AdmissionPass::Min, true) == MIN,
                 "the second pass lets a request that may block down to Min");
    checks.check(ZONE.floor(AdmissionPass::Min, false) == MIN - MIN / 4,
                 "the second pass lets a request that cannot block a quarter below Min");
    checks.check(Watermarks{}.floor(AdmissionPass::Low, true) == 0 &&
                     Watermarks{}.floor(AdmissionPass::Min, false) == 0,
                 "with no watermarks set yet, nothing is held back");
}

auto counts(SmokeChecks &checks) -> void {
    checks.check(admits(LOW + 1, 0, LOW), "a frame out of Low + 1 leaves Low");
    checks.check(!admits(LOW, 0, LOW), "a frame out of Low would leave Low - 1");
    checks.check(admits(LOW + 8, 3, LOW) && !admits(LOW + 7, 3, LOW),
                 "a block of eight counts as eight frames");
    checks.check(!admits(3, 2, 0),
                 "a block larger than the free count is refused even with no floor");
    checks.check(admits(1, 0, 0), "with no floor the last frame goes");
}

/// The four rows of the worked example in the design: one zone, the free
/// count falling, the request an ordinary one that may block.
auto walk_down(SmokeChecks &checks) -> void {
    auto const first = [](usize const free) {
        return admits(free, 0, ZONE.floor(AdmissionPass::Low, true));
    };
    auto const second = [](usize const free) {
        return admits(free, 0, ZONE.floor(AdmissionPass::Min, true));
    };
    checks.check(first(900), "at 900 free the first pass admits");
    checks.check(!first(800) && second(800), "at 800 the first pass refuses and the second admits");
    checks.check(!first(MIN) && !second(MIN), "at Min both passes refuse a request that may block");
    checks.check(admits(MIN, 0, ZONE.floor(AdmissionPass::Min, false)),
                 "at Min a request that cannot block is still admitted");
}

/// The reserve on the default machine (Dma 3388 frames, Dma32 122515, no
/// Normal) and on one with 640150 frames of Normal above that (the 3 GiB
/// machine while Dma32 ended at 512 MiB), by type index: Dma 0, Dma32 1,
/// Normal 2, and two types above that no node has here.
auto reserves(SmokeChecks &checks) -> void {
    constexpr auto RATIO = usize{256};
    constexpr usize SMALL[] = {3388, 122515, 0, 0, 0};
    constexpr usize LARGE[] = {3388, 122515, 640150, 0, 0};
    constexpr auto COUNT = usize{5};

    checks.check(
        lowmem_reserve(SMALL, COUNT, 0, 2, RATIO) == 478,
        "the default machine's Dma keeps 122515 / 256 frames from a request that allows Normal");
    checks.check(lowmem_reserve(SMALL, COUNT, 1, 2, RATIO) == 0,
                 "with no Normal zone, Dma32 keeps nothing back");
    checks.check(lowmem_reserve(SMALL, COUNT, 0, 0, RATIO) == 0,
                 "a request that allows nothing above Dma meets no reserve there");
    checks.check(lowmem_reserve(LARGE, COUNT, 0, 2, RATIO) == (122515 + 640150) / 256 &&
                     lowmem_reserve(LARGE, COUNT, 1, 2, RATIO) == 640150 / 256,
                 "on 3 GiB both low zones keep a share of everything above them");
    checks.check(lowmem_reserve(LARGE, COUNT, 0, 1, RATIO) == 122515 / 256,
                 "a request that stops at Dma32 counts only Dma32 against Dma");
    checks.check(lowmem_reserve(LARGE, COUNT, 2, 2, RATIO) == 0,
                 "the highest zone keeps nothing back");

    // On the default machine a Dma zone at Low + reserve refuses a request
    // that allows Normal, and admits one that allows only Dma.
    constexpr auto DMA_LOW = usize{23};
    constexpr auto RESERVE = usize{478};
    checks.check(!admits(DMA_LOW + RESERVE, 0, DMA_LOW + RESERVE) &&
                     admits(DMA_LOW + RESERVE, 0, DMA_LOW),
                 "the reserve sits on top of the floor");
}

} // namespace

auto main() -> int {
    SmokeChecks checks{"admission-smoke"};
    floors(checks);
    counts(checks);
    walk_down(checks);
    reserves(checks);

    return checks.verdict();
}
