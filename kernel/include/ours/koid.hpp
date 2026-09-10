/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_KOID_HPP
#define OURS_KOID_HPP 1

#include <ours/types.hpp>

#include <ustl/sync/atomic.hpp>

namespace ours {
/// Reserved: KoIdGenerator never hands this out, so callers can use it to
/// mean "no object".
constexpr KoId KOID_INVALID = 0;

class KoIdGenerator {
    using Self = KoIdGenerator;

  public:
    static auto generate(KoId count = 1) -> KoId {
        return TRACKER.fetch_add(count, ustl::sync::MemoryOrder::Relaxed);
    }

  private:
    static inline ustl::sync::Atomic<KoId> TRACKER{KOID_INVALID + 1};
};
} // namespace ours

#endif // #ifndef OURS_KOID_HPP
