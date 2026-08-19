#ifndef OURS_SYNC_SEQLOCK_HPP
#define OURS_SYNC_SEQLOCK_HPP 1

#include <ours/types.hpp>

#include <ustl/sync/atomic.hpp>

namespace ours::sync {
    class SeqLock {
        using Self = SeqLock;
      public:
        ustl::sync::Atomic<u64> m_;
    };
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SEQLOCK_HPP
