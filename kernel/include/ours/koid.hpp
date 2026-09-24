/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_KOID_HPP
#define OURS_KOID_HPP 1

#include <ours/types.hpp>

#include <atomic>

namespace ours {
/// Reserved: KoIdGenerator never hands this out, so callers can use it to
/// mean "no object".
constexpr KoId KOID_INVALID = 0;

class KoIdGenerator {
    using Self = KoIdGenerator;

  public:
    /// Reserves |count| consecutive ids and returns the first. A count, not
    /// an id, so a usize.
    static auto generate(usize count = 1) -> KoId {
        return TRACKER.fetch_add(count, std::memory_order_relaxed);
    }

  private:
    static inline std::atomic<KoId> TRACKER{KOID_INVALID + 1};
};
} // namespace ours

#endif // #ifndef OURS_KOID_HPP
