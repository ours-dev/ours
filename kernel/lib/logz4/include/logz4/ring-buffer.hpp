/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOGZ4_RING_BUFFER_HPP
#define LOGZ4_RING_BUFFER_HPP 1

#include <logz4/types.hpp>

namespace logz4 {
    class RingBuffer {
        using Self = RingBuffer;
      public:
        auto produce() -> bool;
        auto consume() -> void;
      private:
        struct Descriptor {
            enum class State {
                Miss,
                /// The record has been committed but is modifiable by writer.
                Committed,
                /// A writer is modifing the record. Set in reopen().
                Reserved,
                /// The record has been committed and is not modifiable by writer.
                Finalized,
                Reusable,
            };
            usize state;
            usize begin;
            usize next;
        };

        struct DescRing {
            Descriptor *begin;
            usize head_id;
            usize tail_id;
        };

        struct DataRing {
            usize head_begin;
            usize head_end;
            usize tail_begin;
            usize tail_end;
            char *data;
        };

        DescRing m_desc_ring;
        DataRing m_data_ring;
    };
} // namespace logz4

#endif // #ifndef LOGZ4_RING-BUFFER_HPP