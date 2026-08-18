/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_BITFIELDS_HPP
#define USTL_BITFIELDS_HPP 1

#include <cstddef>
#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>

namespace ustl {
namespace util {

template <std::size_t Size>
struct FixedString {
    char value[Size] {};

    constexpr FixedString(char const (&source)[Size]) {
        for (std::size_t index = 0; index < Size; ++index) {
            value[index] = source[index];
        }
    }

    constexpr auto operator==(FixedString const &) const -> bool = default;
};

} // namespace util

namespace bitfields {

template <auto Value>
struct Id {
    static constexpr auto kId = Value;
};

template <std::size_t Value>
struct Bits {
    static constexpr std::size_t kBits = Value;
};

template <std::size_t Value>
struct StartBit {
    static constexpr std::size_t kStartBit = Value;
};

template <bool Value>
struct Enable {
    static constexpr bool kEnable = Value;
};

template <bool Value>
struct Natural {
    static constexpr bool kNatural = Value;
};

template <typename Value>
struct Type {
    using ValueType = Value;
};

template <util::FixedString Value>
struct Name {};

template <typename Value>
struct StorageUnit {
    using StorageUnitType = Value;
};

namespace detail {

inline constexpr std::size_t kAutomaticStartBit = std::numeric_limits<std::size_t>::max();

template <typename... Options>
struct HasId : std::false_type {};

template <auto Value, typename... Options>
struct HasId<Id<Value>, Options...> : std::true_type {};

template <typename Option, typename... Options>
struct HasId<Option, Options...> : HasId<Options...> {};

template <auto Default, typename... Options>
struct IdValue {
    static constexpr auto kValue = Default;
};

template <auto Default, auto Value, typename... Options>
struct IdValue<Default, Id<Value>, Options...> {
    static constexpr auto kValue = Value;
};

template <auto Default, typename Option, typename... Options>
struct IdValue<Default, Option, Options...> : IdValue<Default, Options...> {};

template <std::size_t Default, typename... Options>
struct BitsValue {
    static constexpr std::size_t kValue = Default;
};

template <std::size_t Default, std::size_t Value, typename... Options>
struct BitsValue<Default, Bits<Value>, Options...> {
    static constexpr std::size_t kValue = Value;
};

template <std::size_t Default, typename Option, typename... Options>
struct BitsValue<Default, Option, Options...> : BitsValue<Default, Options...> {};

template <std::size_t Default, typename... Options>
struct StartBitValue {
    static constexpr std::size_t kValue = Default;
};

template <std::size_t Default, std::size_t Value, typename... Options>
struct StartBitValue<Default, StartBit<Value>, Options...> {
    static constexpr std::size_t kValue = Value;
};

template <std::size_t Default, typename Option, typename... Options>
struct StartBitValue<Default, Option, Options...> : StartBitValue<Default, Options...> {};

template <bool Default, typename... Options>
struct EnableValue {
    static constexpr bool kValue = Default;
};

template <bool Default, bool Value, typename... Options>
struct EnableValue<Default, Enable<Value>, Options...> {
    static constexpr bool kValue = Value;
};

template <bool Default, typename Option, typename... Options>
struct EnableValue<Default, Option, Options...> : EnableValue<Default, Options...> {};

template <bool Default, typename... Options>
struct NaturalValue {
    static constexpr bool kValue = Default;
};

template <bool Default, bool Value, typename... Options>
struct NaturalValue<Default, Natural<Value>, Options...> {
    static constexpr bool kValue = Value;
};

template <bool Default, typename Option, typename... Options>
struct NaturalValue<Default, Option, Options...> : NaturalValue<Default, Options...> {};

template <typename Default, typename... Options>
struct TypeValue {
    using Type = Default;
};

template <typename Default, typename Value, typename... Options>
struct TypeValue<Default, Type<Value>, Options...> {
    using Type = Value;
};

template <typename Default, typename Option, typename... Options>
struct TypeValue<Default, Option, Options...> : TypeValue<Default, Options...> {};

template <typename T>
struct IsField : std::false_type {};

template <typename T>
struct IsStorageUnit : std::false_type {};

template <typename T>
struct IsStorageUnit<StorageUnit<T>> : std::true_type {};

template <std::size_t Cursor, typename... Fields>
struct LayoutExtent;

template <std::size_t Cursor>
struct LayoutExtent<Cursor> {
    static constexpr std::size_t kValue = Cursor;
};

template <std::size_t Cursor, typename Field, typename... Fields>
struct LayoutExtent<Cursor, Field, Fields...> {
    static constexpr std::size_t kStart = Field::kStartBit == kAutomaticStartBit ? Cursor : Field::kStartBit;
    static_assert(kStart >= Cursor, "bit field start bit overlaps a preceding field");
    static constexpr std::size_t kEnd = kStart + (Field::kEnable ? Field::kBits : 0);
    static constexpr std::size_t kValue = LayoutExtent<kEnd, Fields...>::kValue;
};

template <auto Value>
struct MissingId {};

template <typename>
inline constexpr bool kAlwaysFalse = false;

template <auto Query, std::size_t Cursor, typename... Fields>
struct LocatedField;

template <auto Query, std::size_t Cursor>
struct LocatedField<Query, Cursor> {
    static_assert(kAlwaysFalse<MissingId<Query>>, "bit field ID does not exist");
};

template <bool Matches, auto Query, std::size_t Start, std::size_t NextCursor, typename Field,
          typename... Remaining>
struct LocatedFieldSelect;

template <auto Query, std::size_t Start, std::size_t NextCursor, typename Field, typename... Remaining>
struct LocatedFieldSelect<true, Query, Start, NextCursor, Field, Remaining...> {
    using Type = Field;
    static constexpr std::size_t kOffset = Start;
};

template <auto Query, std::size_t Start, std::size_t NextCursor, typename Field, typename... Remaining>
struct LocatedFieldSelect<false, Query, Start, NextCursor, Field, Remaining...>
    : LocatedField<Query, NextCursor, Remaining...> {};

template <auto Query, std::size_t Cursor, typename Field, typename... Remaining>
struct LocatedField<Query, Cursor, Field, Remaining...>
    : LocatedFieldSelect<Query == Field::kId, Query,
                          Field::kStartBit == kAutomaticStartBit ? Cursor : Field::kStartBit,
                          (Field::kStartBit == kAutomaticStartBit ? Cursor : Field::kStartBit)
                              + (Field::kEnable ? Field::kBits : 0),
                          Field, Remaining...> {};

template <typename Field, typename... Remaining>
consteval auto HasDuplicateId() -> bool {
    return ((Field::kId == Remaining::kId) || ... || false);
}

template <typename... Fields>
struct HasDuplicateIds;

template <>
struct HasDuplicateIds<> : std::false_type {};

template <typename Field, typename... Remaining>
struct HasDuplicateIds<Field, Remaining...>
    : std::bool_constant<HasDuplicateId<Field, Remaining...>() || HasDuplicateIds<Remaining...>::value> {};

template <std::size_t Bits>
struct DefaultStorage {
    using Type = std::conditional_t<
        Bits <= 8, std::uint8_t,
        std::conditional_t<Bits <= 16, std::uint16_t,
                           std::conditional_t<Bits <= 32, std::uint32_t, std::uint64_t>>>;
};

template <typename Storage, typename... Fields>
class BitFieldsImpl {
    using Self = BitFieldsImpl;

    static_assert((IsField<Fields>::value && ...), "BitFields accepts only Field descriptors");
    static_assert(!HasDuplicateIds<Fields...>::value, "BitFields has duplicated field IDs");
    static_assert(std::is_integral_v<Storage> && std::is_unsigned_v<Storage>,
                  "BitFields storage must be an unsigned integer");

    template <auto Id>
    using FieldFor = typename LocatedField<Id, 0, Fields...>::Type;

    template <auto Id>
    static constexpr std::size_t kOffsetFor = LocatedField<Id, 0, Fields...>::kOffset;

    static constexpr std::size_t kStorageBits = std::numeric_limits<Storage>::digits;

    template <std::size_t Bits>
    static consteval auto mask() -> Storage {
        static_assert(Bits > 0 && Bits <= kStorageBits, "bit field does not fit in its storage unit");
        if constexpr (Bits == kStorageBits) {
            return ~Storage {0};
        } else {
            return static_cast<Storage>((Storage {1} << Bits) - 1);
        }
    }

    template <typename Value>
    static constexpr auto encode(Value value) noexcept -> Storage {
        if constexpr (std::is_pointer_v<Value>) {
            return static_cast<Storage>(reinterpret_cast<std::uintptr_t>(value));
        } else {
            return static_cast<Storage>(value);
        }
    }

    template <typename Value>
    static constexpr auto decode(Storage value) noexcept -> Value {
        if constexpr (std::is_pointer_v<Value>) {
            return reinterpret_cast<Value>(static_cast<std::uintptr_t>(value));
        } else {
            return static_cast<Value>(value);
        }
    }

    template <auto Id>
    constexpr auto set_value(typename FieldFor<Id>::ValueType const &value) noexcept -> void {
        write_value<Id>(value_, value);
    }

    template <auto Id>
    constexpr auto set_value(typename FieldFor<Id>::ValueType const &value) volatile noexcept -> void {
        write_value<Id>(value_, value);
    }

    template <auto Id, typename StorageRef>
    static constexpr auto write_value(StorageRef &storage, typename FieldFor<Id>::ValueType const &value) noexcept
        -> void {
        using Field = FieldFor<Id>;
        if constexpr (Field::kEnable) {
            constexpr auto field_mask = mask<Field::kBits>();
            constexpr auto offset = kOffsetFor<Id>;
            static_assert(offset + Field::kBits <= kStorageBits, "bit field does not fit in its storage unit");
            constexpr auto shifted_mask = static_cast<Storage>(field_mask << offset);
            if constexpr (Field::kNatural) {
                // The field's value is already positioned at its final bit
                // offset (e.g. a page-aligned address) — mask it in place
                // rather than shifting it into position.
                auto const masked_value = static_cast<Storage>(encode(value) & shifted_mask);
                storage = static_cast<Storage>((storage & ~shifted_mask) | masked_value);
            } else {
                auto const masked_value = static_cast<Storage>(encode(value) & field_mask);
                storage = static_cast<Storage>((storage & ~shifted_mask) | (masked_value << offset));
            }
        }
    }

  public:
    using StorageUnit = Storage;

    static constexpr std::size_t NUM_BITS = LayoutExtent<0, Fields...>::kValue;
    static constexpr std::size_t NUM_UNITS = 1;
    static_assert(NUM_BITS <= kStorageBits, "bit fields span more than one storage unit");

    template <auto Id>
    static constexpr bool IsFieldEnabledV = FieldFor<Id>::kEnable;

    template <auto Id>
    using ValueTypeOf = typename FieldFor<Id>::ValueType;

    constexpr BitFieldsImpl() = default;

    template <auto Id>
    constexpr auto set(ValueTypeOf<Id> const &value) noexcept -> Self & {
        set_value<Id>(value);
        return *this;
    }

    template <auto Id>
    constexpr auto set(ValueTypeOf<Id> const &value) volatile noexcept -> Self volatile & {
        set_value<Id>(value);
        return *this;
    }

    template <auto Id>
    constexpr auto get() const noexcept -> ValueTypeOf<Id> {
        return read_value<Id>(value_);
    }

    template <auto Id>
    constexpr auto get() const volatile noexcept -> ValueTypeOf<Id> {
        return read_value<Id>(value_);
    }

  private:
    template <auto Id, typename StorageRef>
    static constexpr auto read_value(StorageRef const &storage) noexcept -> ValueTypeOf<Id> {
        using Field = FieldFor<Id>;
        if constexpr (!Field::kEnable) {
            return {};
        } else {
            constexpr auto offset = kOffsetFor<Id>;
            constexpr auto field_mask = mask<Field::kBits>();
            if constexpr (Field::kNatural) {
                constexpr auto shifted_mask = static_cast<Storage>(field_mask << offset);
                return decode<ValueTypeOf<Id>>(static_cast<Storage>(storage & shifted_mask));
            } else {
                return decode<ValueTypeOf<Id>>(static_cast<Storage>((storage >> offset) & field_mask));
            }
        }
    }

  public:
    template <auto Id>
    constexpr auto add(ValueTypeOf<Id> delta) volatile noexcept -> ValueTypeOf<Id> {
        using Field = FieldFor<Id>;
        static_assert(Field::kBits > 1 && std::is_arithmetic_v<ValueTypeOf<Id>>,
                      "add requires an arithmetic field wider than one bit");
        auto const old = get<Id>();
        set<Id>(static_cast<ValueTypeOf<Id>>(old + delta));
        return old;
    }

    template <auto Id>
    constexpr auto sub(ValueTypeOf<Id> delta) volatile noexcept -> ValueTypeOf<Id> {
        using Field = FieldFor<Id>;
        static_assert(Field::kBits > 1 && std::is_arithmetic_v<ValueTypeOf<Id>>,
                      "sub requires an arithmetic field wider than one bit");
        auto const old = get<Id>();
        set<Id>(static_cast<ValueTypeOf<Id>>(old - delta));
        return old;
    }

    template <typename Integer>
    constexpr auto into() const noexcept -> Integer {
        static_assert(std::is_integral_v<Integer> && sizeof(Integer) >= sizeof(Storage),
                      "target integer cannot represent this BitFields storage");
        return static_cast<Integer>(value_);
    }

    static constexpr auto from_raw(Storage raw) noexcept -> Self {
        Self result;
        result.value_ = raw;
        return result;
    }

  private:
    Storage value_ {};
};

template <typename... Args>
struct MakeBitFields {
    using Storage = typename DefaultStorage<LayoutExtent<0, Args...>::kValue>::Type;
    using Type = BitFieldsImpl<Storage, Args...>;
};

template <typename Storage, typename... Fields>
struct MakeBitFields<StorageUnit<Storage>, std::tuple<Fields...>> {
    using Type = BitFieldsImpl<Storage, Fields...>;
};

} // namespace detail

template <typename... Options>
struct Field {
    static_assert(detail::HasId<Options...>::value, "each Field needs an Id option");

    using ValueType = typename detail::TypeValue<std::size_t, Options...>::Type;
    static constexpr auto kId = detail::IdValue<std::size_t {0}, Options...>::kValue;
    static constexpr std::size_t kBits = detail::BitsValue<1, Options...>::kValue;
    static constexpr std::size_t kStartBit =
        detail::StartBitValue<detail::kAutomaticStartBit, Options...>::kValue;
    static constexpr bool kEnable = detail::EnableValue<true, Options...>::kValue;
    static constexpr bool kNatural = detail::NaturalValue<false, Options...>::kValue;

    static_assert(kBits > 0, "a bit field must contain at least one bit");
};

namespace detail {

template <typename... Options>
struct IsField<Field<Options...>> : std::true_type {};

} // namespace detail

// Convenience alias for the common case where a field's Id (typically an
// enum whose value is the bit's position, e.g. `Cr0Bits::Wp = 16`) already
// says where the field starts — StartBit doesn't need to be repeated as a
// separate literal that can drift out of sync with it. Defaults to a
// single boolean flag bit; pass Bits/ValueType/IsNatural for wider or
// differently-typed fields (e.g. `FieldAt<Cr3Bits::PageTableAddress, 40,
// std::size_t, /*IsNatural=*/true>`).
template <auto Id, std::size_t Bits = 1, typename ValueType = bool, bool IsNatural = false>
using FieldAt = Field<bitfields::Id<Id>, bitfields::Bits<Bits>,
                       bitfields::StartBit<static_cast<std::size_t>(Id)>,
                       bitfields::Type<ValueType>, bitfields::Natural<IsNatural>>;

template <typename... Args>
using BitFields = typename detail::MakeBitFields<Args...>::Type;

} // namespace bitfields

using bitfields::BitFields;
using bitfields::Field;
using bitfields::FieldAt;

} // namespace ustl

#endif // USTL_BITFIELDS_HPP
