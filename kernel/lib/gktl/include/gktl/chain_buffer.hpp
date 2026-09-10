/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_CHAIN_BUFFER_HPP
#define GKTL_CHAIN_BUFFER_HPP 1

#include <gktl/types.hpp>

#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/option.hpp>
#include <ustl/util/pair.hpp>

namespace gktl {
namespace buffer {
    class SingleBufferCollection {
        struct SingleBuffer: public ustl::collections::intrusive::SlistBaseHook<> {
            u8 *buffer;
        };
        using SingleBufferList = ustl::collections::intrusive::Slist<SingleBuffer>;

      public:
        class Cursor;

      private:
        usize m_single_buffer_capacity;
        SingleBufferList m_inner;
    };

    class SingleBufferCollection::Cursor {
        using Self = SingleBufferCollection::Cursor;

      public:
        template <typename Integer>
        auto write(Integer integer) -> bool {}

        template <typename Integer>
        auto read() -> Integer;

        auto skip(usize num_bytes) -> void;

      private:
        usize m_offset;
        usize m_local_offset;
        SingleBufferCollection *m_collection;
        SingleBufferList::IterMut m_iter;
    };

} // namespace buffer
class ChainBuffer {
    using Self = ChainBuffer;

  public:
    class Writer;
    class Reader;

    constexpr static auto DEFAULT_SINGLE_BUFFER_CAPACITY = 4096;

    static auto create(usize initial_nodes, usize capacity) -> ustl::Pair<Writer &, Reader &>;

    auto set_memory_throttle(usize throttle) -> usize {
        return std::exchange(m_memory_throttle, throttle);
    }

    auto memory_throttle() const -> usize {
        return m_memory_throttle;
    }

    auto memory_capacity() const -> usize {
        return m_memory_capacity;
    }

    auto size() const -> usize {
        return m_collection;
    }

    auto writer() -> Writer &;
    auto reader() -> Reader &;

  private:
    usize m_memory_throttle = 0;
    usize m_memory_capacity = 0;
    usize m_single_buffer_capacity = DEFAULT_SINGLE_BUFFER_CAPACITY;
    buffer::SingleBufferCollection m_collection;
    buffer::SingleBufferCollection::Cursor m_writer_cursor;
    buffer::SingleBufferCollection::Cursor m_reader_cursor;
};

class ChainBuffer::Reader: public ChainBuffer {
  public:
    auto read(u8 *buffer, usize len) -> usize;
};
static_assert(sizeof(ChainBuffer) == sizeof(ChainBuffer::Reader));

class ChainBuffer::Writer: public ChainBuffer {
  public:
    auto write(u8 *buffer, usize len) -> usize;
};
static_assert(sizeof(ChainBuffer) == sizeof(ChainBuffer::Writer));

inline auto ChainBuffer::writer() -> Writer & {
    return static_cast<Writer &>(*this);
}

inline auto ChainBuffer::reader() -> Reader & {
    return static_cast<Reader &>(*this);
}
} // namespace gktl

#endif // #ifndef GKTL_CHAIN-BUFFER_HPP