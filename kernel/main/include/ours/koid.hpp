/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_KOID_HPP
#define OURS_KOID_HPP 1

#include <ours/types.hpp>

#include <atomic> // std::atomic<T>

namespace ours {
    class KoIdGenerator {
        using Self = KoIdGenerator;
      public:
        static auto generate(usize count = 1) -> KoId {
            return g_tracker.fetch_add(count, std::memory_order_relaxed);
        }
      private:
        static inline std::atomic<KoId> g_tracker{0};
    };
} // namespace ours

#endif // #ifndef OURS_KOID_HPP