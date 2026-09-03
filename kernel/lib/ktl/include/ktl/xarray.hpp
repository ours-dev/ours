/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_XARRAY_HPP
#define KTL_XARRAY_HPP 1

#include <ktl/result.hpp>
#include <ours/types.hpp>
#include <ustl/bitfields.hpp>
#include <ustl/bitset.hpp>
#include <ustl/option.hpp>

#include <array>
#include <limits>

namespace ktl {
namespace xarray {
    template <auto Id, typename Type, usize Bits>
    using Field = ustl::bitfields::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Type<Type>,
                                         ustl::bitfields::Bits<Bits>>;

    struct XArryDefaultConfig {
        constexpr static auto SLOTS_BITS = 6;

        constexpr static auto NUM_BIT_TAGS = 4;
    };

    template <typename A, typename C = XArryDefaultConfig>
    class XArray {
        using Self = XArray;

      public:
        constexpr static auto NUM_BIT_TAGS = C::NUM_BIT_TAGS;

        constexpr static auto SLOTS_BITS = C::SLOTS_BITS;

        constexpr static auto SLOTS_MASK = (1 << SLOTS_BITS) - 1;

        constexpr static auto MAX_SLOTS = (1 << SLOTS_BITS);

        class Cursor;

        struct Node;
        struct Entry {
            enum class Type {
                Empty,
                Pointer,
                Internal,
                Value,
                ValueTagged,
                MaxNumType,
            };

            constexpr static auto TYPE_BITS = ustl::bit_width(usize(Type::MaxNumType));

            constexpr static auto VALUE_BITS = std::numeric_limits<usize>::digits - TYPE_BITS;

            using Inner = ustl::BitFields<Field<0, Type, TYPE_BITS>, Field<1, usize, VALUE_BITS>>;

            FORCE_INLINE constexpr auto max_index() const -> usize {
                if (is_node()) {
                    return value<Node>().max_index();
                }
                return 0;
            }

            FORCE_INLINE constexpr auto type() const -> Type {
                return inner.template get<0>();
            }

            FORCE_INLINE constexpr auto set_type(Type type) -> Self & {
                inner.template set<0>(type);
                return *this;
            }

            FORCE_INLINE constexpr auto is_value() const -> bool {
                return type() == Type::ValueTagged;
            }

            FORCE_INLINE constexpr auto is_node() const -> bool {
                return type() == Type::Internal;
            }

            FORCE_INLINE constexpr auto is_null() const -> bool {
                return reinterpret_cast<usize>(inner) == 0;
            }

            template <typename T>
            FORCE_INLINE constexpr auto value() -> T & {
                static_assert(sizeof(T) == sizeof(usize), "");
                return reinterpret_cast<T &>(inner.template get<1>());
            }

            template <typename T>
            FORCE_INLINE constexpr auto cast_to() -> ustl::Option<T> {
                static_assert(sizeof(T) == sizeof(usize), "");
                if (!is_value()) {
                    return ustl::none();
                }
                return *reinterpret_cast<T *>(reinterpret_cast<usize>(inner.template get<1>()));
            }

            FORCE_INLINE constexpr auto node() -> Node * {
                return value<Node>();
            }

            FORCE_INLINE constexpr auto value() const -> usize {
                return inner.template get<1>();
            }

            FORCE_INLINE constexpr auto set_value(usize value) -> Self & {
                inner.template set<1>(value);
                return *this;
            }

            template <typename T>
            FORCE_INLINE static auto make(Type type, T *object) -> Entry {
                return Entry().set_type(type).set_value(reinterpret_cast<usize>(object));
            }

            template <typename T>
            FORCE_INLINE static auto make_node(T *object) -> Entry {
                return make(Type::Internal, object);
            }

            Inner inner;
        };
        using Slots = std::array<Entry, MAX_SLOTS>;

        struct Node {
            auto get_offset(usize index) const -> usize {
                return (index >> shift) & SLOTS_MASK;
            }

            auto get_entry(usize index) const -> Entry {
                return slots[get_offset(index)];
            }

            auto set_entry(usize index, Entry entry) const -> Self & {
                slots[get_offset(index)] = entry;
                return *this;
            }

            auto max_index() const -> usize {
                return (MAX_SLOTS << shift) - 1;
            }

            u8 shift;  // The order of number of slots, if zero the slot is a leaf node.
            u8 offset; // Slot offfset in parent.
            u8 nr_inuses;
            u8 nr_values;
            Node *parent;
            XArray *owner;
            Slots slots;
        };
        typedef Node *NodePtrMut;
        typedef Node const *NodePtr;
        typedef typename A::template RebindT<Node> Allocator;

        auto make_cursor(usize index, usize shift) -> Cursor {
            return Cursor(this, index, shift, 0, 0, Cursor::ROOT_TAG, 0);
        }

        auto load(usize index) -> Entry;

        auto store(usize index, Entry entry) -> ktl::Result<ustl::Option<Entry>>;

        auto erase(usize index) -> Entry;

        template <typename T>
        static auto make_entry(T *object) -> Entry;

      private:
        auto alloc_node() -> NodePtrMut {
            return payload_.get_allocator().allocate();
        }

        struct Payload: public Allocator {
            auto get_allocator() -> Allocator & {
                return *this;
            }

            Entry entry;
        } payload_;
    };

    template <typename A, typename C>
    class XArray<A, C>::Cursor {
        using Self = Cursor;

      public:
      private:
        XArray *m_owner;
        usize m_index;
        usize m_shift;
        usize m_offset;
        NodePtrMut m_node; // Node we are operating on.
        ustl::BitSet<MAX_SLOTS * NUM_BIT_TAGS> m_tags;
    };

} // namespace xarray
} // namespace ktl

#endif // #ifndef KTL_XARRAY_HPP