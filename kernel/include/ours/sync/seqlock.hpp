#ifndef OURS_SYNC_SEQLOCK_HPP
#define OURS_SYNC_SEQLOCK_HPP 1

#include <atomic>

namespace ours::sync {
    class SeqLock {
        using Self = SeqLock;
      public:
        std::atomic<u64> m_;
    };
} // namespace ours::sync

#endif // #ifndef OURS_SYNC_SEQLOCK_HPP