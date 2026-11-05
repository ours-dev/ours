/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/assert.hpp>
#include <ours/mm/priv/alloc_failure.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

namespace ours::mm::test {
namespace {
    // Readers compare the thread before accessing the scope. A scope belongs
    // to that thread until destruction, even if it blocks or migrates.
    std::atomic<task::Thread *> OWNER{nullptr};
    AllocationFailure *ACTIVE_FAILURE = nullptr;
} // namespace

AllocationFailure::AllocationFailure(AllocationSite const site, usize const skip)
    : m_site(site), m_skip(skip) {
    auto *const current = task::Thread::Current::get();
    task::Thread *unowned = nullptr;
    ASSERT(current && OWNER.compare_exchange_strong(unowned, current, std::memory_order_acquire),
           "an allocation failure scope is already active");
    ACTIVE_FAILURE = this;
}

AllocationFailure::~AllocationFailure() {
    ASSERT(ACTIVE_FAILURE == this &&
               OWNER.load(std::memory_order_relaxed) == task::Thread::Current::get(),
           "allocation failure scope must end on its owning thread");
    ACTIVE_FAILURE = nullptr;
    OWNER.store(nullptr, std::memory_order_release);
}

auto AllocationFailure::hit(AllocationSite const site) -> bool {
    auto *const owner = OWNER.load(std::memory_order_relaxed);
    if (!owner || task::Thread::Current::get() != owner) {
        return false;
    }
    auto *const failure = ACTIVE_FAILURE;
    if (!failure || failure->m_site != site || failure->m_triggered) {
        return false;
    }
    if (failure->m_skip != 0) {
        --failure->m_skip;
        return false;
    }
    failure->m_triggered = true;
    return true;
}

} // namespace ours::mm::test
