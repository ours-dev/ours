/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_KOID_HPP
#define OURS_KOID_HPP 1

#include <ours/types.hpp>

#include <ustl/sync/atomic.hpp>

namespace ours {
    class KoIdGenerator {
        using Self = KoIdGenerator;
      public:
        static auto generate(usize count = 1) -> KoId {
            return g_tracker.fetch_add(count, ustl::sync::MemoryOrder::Relaxed);
        }
      private:
        static inline ustl::sync::Atomic<KoId> g_tracker {0};
    };
} // namespace ours

#endif // #ifndef OURS_KOID_HPP
