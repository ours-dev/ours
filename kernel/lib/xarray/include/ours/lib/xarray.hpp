/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_LIB_XARRAY_HPP
#define OURS_LIB_XARRAY_HPP 1

#include <ours/types.hpp>
#include <ours/status.hpp>

namespace ours::lib {
    /// A sparse, pointer-valued array modeled after Linux XArray.
    ///
    /// Users address objects by index. Tagged node entries stay private to the
    /// container implementation.
    class XArray {
        using Self = XArray;
        struct Node;
      public:
        using Index = usize;
        using Object = void;
        using Order = u8;

        enum class Alloc {
            None,
            Zero,
        };

        enum class StoreMode {
            Create,
            Replace,
            Update,
        };

        enum class Mark : u8 {
            Mark0,
            Mark1,
            Mark2,
        };

        class Cursor {
            using Self = Cursor;
          public:
            constexpr explicit Cursor(Index index = 0, Order order = 0)
                : m_index(index),
                  m_order(order) {}

            constexpr auto index() const -> Index {
                return m_index;
            }

            constexpr auto order() const -> Order {
                return m_order;
            }

            constexpr auto shift() const -> u8 {
                return m_shift;
            }

          private:
            friend class XArray;

            Index m_index {};
            Order m_order {};
            u8 m_shift {};
        };

        XArray() = default;
        ~XArray();

        XArray(Self const &) = delete;
        XArray(Self &&) = delete;
        auto operator=(Self const &) -> Self & = delete;
        auto operator=(Self &&) -> Self & = delete;

        auto load(Index index) const -> Object *;

        template <typename T>
        auto load(Index index) const -> T * {
            return static_cast<T *>(load(index));
        }

        auto store(Index index, Object *object, StoreMode mode = StoreMode::Replace) -> Status;

        template <typename T>
        auto store(Index index, T *object, StoreMode mode = StoreMode::Replace) -> Status {
            return store(index, static_cast<Object *>(object), mode);
        }

        auto erase(Index index) -> Object *;

        auto reserve(Index index, Order order = 0) -> Status;

        auto clear() -> void;

        auto empty() const -> bool;

        auto set_mark(Index index, Mark mark) -> Status;

        auto clear_mark(Index index, Mark mark) -> Status;

        auto marked(Index index, Mark mark) const -> bool;

        auto find(Cursor &cursor) const -> Object *;

        template <typename T>
        auto find(Cursor &cursor) const -> T * {
            return static_cast<T *>(find(cursor));
        }

        auto find_marked(Cursor &cursor, Mark mark) const -> Object *;

        template <typename T>
        auto find_marked(Cursor &cursor, Mark mark) const -> T * {
            return static_cast<T *>(find_marked(cursor, mark));
        }

      private:
        static constexpr auto kChunkShift = 6_u8;
        static constexpr auto kChunkSize = 1_usize << kChunkShift;
        static constexpr auto kChunkMask = kChunkSize - 1;
        static constexpr auto kMaxMarks = 3_usize;

        struct Entry {
            static constexpr auto kInternalMask = 0x3_usize;
            static constexpr auto kNodeTag = 0x2_usize;
            static constexpr auto kRetryTag = 0x3_usize;

            constexpr Entry() = default;

            static constexpr auto null() -> Entry {
                return Entry{};
            }

            static constexpr auto object(Object *object) -> Entry {
                return Entry{usize(object)};
            }

            static auto node(Node *node) -> Entry {
                return Entry{usize(node) | kNodeTag};
            }

            constexpr auto bits() const -> usize {
                return m_bits;
            }

            constexpr auto is_null() const -> bool {
                return m_bits == 0;
            }

            constexpr auto is_node() const -> bool {
                return (m_bits & kInternalMask) == kNodeTag;
            }

            constexpr auto is_retry() const -> bool {
                return (m_bits & kInternalMask) == kRetryTag;
            }

            constexpr auto is_object() const -> bool {
                return !is_null() && (m_bits & kInternalMask) == 0;
            }

            auto as_object() const -> Object * {
                return reinterpret_cast<Object *>(m_bits);
            }

          private:
            explicit constexpr Entry(usize bits)
                : m_bits(bits) {}

            usize m_bits {};
        };

        struct Node {
            Node *parent {};
            u8 shift {};
            u8 offset {};
            u8 count {};
            u8 nr_values {};

            Entry slots[kChunkSize] {};
            u64 marks[kMaxMarks] {};
        };

        static auto slot(Index index, u8 shift) -> usize {
            return (index >> shift) & kChunkMask;
        }

        static auto mark_index(Mark mark) -> usize {
            return static_cast<usize>(mark);
        }

        static auto max_shift(Index index) -> u8;

        static auto node_from_entry(Entry entry) -> Node *;

        static auto create_node(Node *parent, u8 shift, u8 offset) -> Node *;

        static auto destroy_node(Node *node) -> void;

        static auto destroy_entry(Entry entry) -> void;

        static auto descend(Node *node, Index index) -> Entry *;

        static auto descend(Node const *node, Index index) -> Entry const *;

        static auto any_marks(Node const *node) -> u64;

        static auto recalculate_slot_marks(Node *node, usize offset) -> void;

        static auto set_slot_mark(Node *node, usize offset, Mark mark) -> void;

        static auto clear_slot_mark(Node *node, usize offset, Mark mark) -> void;

        auto prune_empty(Node *node) -> void;

        auto ensure_root(Index index) -> Status;

        auto ensure_slot(Index index) -> Entry *;

        auto load_entry(Index index) const -> Entry;

        auto erase_entry(Index index) -> Entry;

        auto find_from(Entry entry, Index base, Index start, Mark const *mark, Cursor &cursor) const -> Entry;

        Entry m_head {};
    };

} // namespace ours::lib

#endif // #ifndef OURS_LIB_XARRAY_HPP
