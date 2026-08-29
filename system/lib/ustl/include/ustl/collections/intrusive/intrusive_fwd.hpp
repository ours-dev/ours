/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_INTRUSIVE_INTRUSIVE_FWD_HPP
#define USTL_COLLECTIONS_INTRUSIVE_INTRUSIVE_FWD_HPP 1

#include <ustl/collections/intrusive/link_mode.hpp>

#include <cstddef>
#include <cstdint>

namespace ustl::collections::intrusive {
using UintPtr = std::uintptr_t;

template <typename NodeTraits>
class CircularListAlgorithms;
template <typename NodeTraits>
class CircularSlistAlgorithms;
template <typename NodeTraits>
class LinearSlistAlgorithms;
template <typename NodeTraits>
class BstreeAlgorithms;
template <typename NodeTraits>
class RBTreeAlgorithms;
template <typename NodeTraits>
class AvltreeAlgorithms;
template <typename NodeTraits>
class SgtreeAlgorithms;
template <typename NodeTraits>
class SplaytreeAlgorithms;
template <typename NodeTraits>
class TreapAlgorithms;

template <typename T, typename... Options>
class Slist;
template <typename... Options>
class SlistBaseHook;
template <typename... Options>
class SlistMemberHook;
template <typename T, typename... Options>
class List;
template <typename... Options>
class ListBaseHook;
template <typename... Options>
class ListMemberHook;
template <typename T, typename... Options>
class RBTree;
template <typename T, typename... Options>
class Set;
template <typename T, typename... Options>
class Multiset;
template <typename... Options>
class SetBaseHook;
template <typename... Options>
class SetMemberHook;
template <typename T, typename... Options>
class Splaytree;
template <typename T, typename... Options>
class SplaySet;
template <typename T, typename... Options>
class SplayMultiset;
template <typename T, typename... Options>
class Avltree;
template <typename T, typename... Options>
class AvlSet;
template <typename T, typename... Options>
class AvlMultiset;
template <typename... Options>
class AvlSetBaseHook;
template <typename... Options>
class AvlSetMemberHook;
template <typename T, typename... Options>
class Treap;
template <typename T, typename... Options>
class TreapSet;
template <typename T, typename... Options>
class TreapMultiset;
template <typename T, typename... Options>
class Sgtree;
template <typename T, typename... Options>
class SgSet;
template <typename T, typename... Options>
class SgMultiset;
template <typename T, typename... Options>
class Bstree;
template <typename T, typename... Options>
class BsSet;
template <typename T, typename... Options>
class BsMultiset;
template <typename... Options>
class BsSetBaseHook;
template <typename... Options>
class BsSetMemberHook;
template <typename T, typename... Options>
class Hashtable;
template <typename T, typename... Options>
class UnorderedSet;
template <typename T, typename... Options>
class UnorderedMultiset;
template <typename... Options>
class UnorderedSetBaseHook;
template <typename... Options>
class UnorderedSetMemberHook;
template <typename... Options>
class AnyBaseHook;
template <typename... Options>
class AnyMemberHook;

template <typename T = void>
struct PriorityCompare;

template <bool Enabled>
struct ConstantTimeSize;
template <typename T>
struct HeaderHolderType;
template <typename T>
struct Size;
template <typename T>
struct Compare;
template <typename T>
struct KeyOfValue;
template <typename T>
struct PriorityOfValue;
template <bool Enabled>
struct FloatingPoint;
template <typename T>
struct Equal;
template <typename T>
struct Priority;
template <typename T>
struct Hash;
template <typename T>
struct ValueTraits;
template <typename Parent, typename TMemberHook, TMemberHook Parent::*PtrMut>
struct MemberHook;
template <typename T>
struct FunctionHook;
template <typename T>
struct BaseHook;
template <typename T>
struct VoidPointer;
template <typename T>
struct Tag;
template <bool Enabled>
struct OptimizeSize;
template <bool Enabled>
struct Linear;
template <bool Enabled>
struct CacheLast;
template <typename T>
struct BucketTraits;
template <bool Enabled>
struct StoreHash;
template <bool Enabled>
struct OptimizeMultikey;
template <bool Enabled>
struct Power2Buckets;
template <bool Enabled>
struct FastmodBuckets;
template <bool Enabled>
struct CacheBegin;
template <bool Enabled>
struct CompareHash;
template <bool Enabled>
struct Incremental;
template <bool Enabled>
struct LinearBuckets;

template <typename T, typename NodeTraits, LinkModeType Mode = LinkModeType::SafeLink>
struct DerivationValueTraits;
template <typename T, typename NodeTraits, typename NodeTraits::Node T::*PtrMut,
          LinkModeType Mode = LinkModeType::SafeLink>
struct MemberValueTraits;
template <typename NodeTraits, LinkModeType Mode = LinkModeType::NormalLink>
struct TrivialValueTraits;

template <typename VoidPointer, std::size_t Alignment>
struct MaxPointerPlusBits;
template <std::size_t Alignment>
struct MaxPointerPlusBits<void *, Alignment>;
template <typename PtrMut, std::size_t NumBits>
struct PointerPlusBits;
template <typename T, std::size_t NumBits>
struct PointerPlusBits<T *, NumBits>;
template <typename PtrMut, typename T>
struct PointerRebind;
template <typename PtrMut>
struct PointerTraits;
template <typename T>
struct PointerTraits<T *>;
} // namespace ustl::collections::intrusive

#endif // USTL_COLLECTIONS_INTRUSIVE_INTRUSIVE_FWD_HPP
