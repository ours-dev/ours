/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Race stress test: Weak::lock() vs the last strong reference being dropped.
/// Run with -fsanitize=thread,address to validate the lock_strong() CAS logic.

#include <atomic>
#include <thread>
#include <vector>

#include <cassert>
#include <cstdio>

#include <ustl/rc.hpp>

// The race test shares one object across threads: it must use the
// thread-safe policy (Arc), not the default single-threaded Rc.
struct Node: ustl::RefCounted<Node, ustl::sync::ThreadSafePolicy> {
    int v = 42;
    ~Node() {
        v = -1;
    } // poison: any successful lock must never see this
};

auto main() -> int {
    std::atomic<bool> stop{false};
    std::atomic<long> ok{0}, fail{0};

    for (int round = 0; round < 200; ++round) {
        ustl::Arc<Node> shared = ustl::Arc<Node>::adopt(new Node());
        auto weak = shared.downgrade();

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    if (auto locked = weak.lock()) {
                        if (locked->v != 42) {
                            std::abort(); // resurrected a destroyed object
                        }
                        ok.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        fail.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        shared = nullptr; // drop the last strong reference
        stop.store(true, std::memory_order_relaxed);
        for (auto &t: threads) {
            t.join();
        }
        assert(!weak.lock()); // object is gone for good
        assert(weak.expired());
        stop.store(false, std::memory_order_relaxed);
    }

    std::printf("ok=%ld fail=%ld\n", ok.load(), fail.load());
    return 0;
}
