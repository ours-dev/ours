/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_FWD_HPP
#define BOOST_INTRUSIVE_FWD_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif
#
#ifndef BOOST_CSTDINT_HPP
#  include <boost/cstdint.hpp>
#endif
#
#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

//! \file
//! This header file forward declares most Intrusive classes.
//!
//! It forward declares the following containers and hooks:
//!   - boost::intrusive::slist / boost::intrusive::slist_base_hook / boost::intrusive::slist_member_hook
//!   - boost::intrusive::list / boost::intrusive::list_base_hook / boost::intrusive::list_member_hook
//!   - boost::intrusive::bstree / boost::intrusive::bs_set / boost::intrusive::bs_multiset /
//!      boost::intrusive::bs_set_base_hook / boost::intrusive::bs_set_member_hook
//!   - boost::intrusive::rbtree / boost::intrusive::set / boost::intrusive::multiset /
//!      boost::intrusive::set_base_hook / boost::intrusive::set_member_hook
//!   - boost::intrusive::avltree / boost::intrusive::avl_set / boost::intrusive::avl_multiset /
//!      boost::intrusive::avl_set_base_hook / boost::intrusive::avl_set_member_hook
//!   - boost::intrusive::splaytree / boost::intrusive::splay_set / boost::intrusive::splay_multiset
//!   - boost::intrusive::sgtree / boost::intrusive::sg_set / boost::intrusive::sg_multiset
//!   - boost::intrusive::treap / boost::intrusive::treap_set / boost::intrusive::treap_multiset
//!   - boost::intrusive::hashtable / boost::intrusive::unordered_set / boost::intrusive::unordered_multiset /
//!      boost::intrusive::unordered_set_base_hook / boost::intrusive::unordered_set_member_hook /
//!   - boost::intrusive::any_base_hook / boost::intrusive::any_member_hook
//!
//! It forward declares the following container or hook options:
//!   - boost::intrusive::constant_time_size / boost::intrusive::size_type / boost::intrusive::compare / boost::intrusive::equal
//!   - boost::intrusive::floating_point / boost::intrusive::priority / boost::intrusive::hash
//!   - boost::intrusive::value_traits / boost::intrusive::member_hook / boost::intrusive::function_hook / boost::intrusive::base_hook
//!   - boost::intrusive::void_pointer / boost::intrusive::tag / boost::intrusive::link_mode
//!   - boost::intrusive::optimize_size / boost::intrusive::linear / boost::intrusive::cache_last
//!   - boost::intrusive::bucket_traits / boost::intrusive::store_hash / boost::intrusive::optimize_multikey
//!   - boost::intrusive::power_2_buckets / boost::intrusive::cache_begin / boost::intrusive::compare_hash / boost::intrusive::incremental
//!
//! It forward declares the following value traits utilities:
//!   - boost::intrusive::value_traits / boost::intrusive::derivation_value_traits /
//!      boost::intrusive::trivial_value_traits
//!
//! Finally it forward declares the following general purpose utilities:
//!   - boost::intrusive::pointer_plus_bits / boost::intrusive::priority_compare.

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

#include <cstddef>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/workaround.hpp>

namespace boost {
namespace intrusive {

#if !defined(BOOST_INTERPROCESS_DOXYGEN_INVOKED)
#  ifdef BOOST_HAS_INTPTR_T
      using ::boost::uintptr_t;
#  else
      typedef std::size_t uintptr_t;
#  endif
#endif

////////////////////////////
//     Node algorithms
////////////////////////////

//Algorithms predeclarations
template<class NodeTraits>
class CircularListAlgorithms;

template<class NodeTraits>
class CircularSlistAlgorithms;

template<class NodeTraits>
class LinearSlistAlgorithms;

template<class NodeTraits>
class BstreeAlgorithms;

template<class NodeTraits>
class RbtreeAlgorithms;

template<class NodeTraits>
class AvltreeAlgorithms;

template<class NodeTraits>
class SgtreeAlgorithms;

template<class NodeTraits>
class SplaytreeAlgorithms;

template<class NodeTraits>
class TreapAlgorithms;

////////////////////////////
//       Containers
////////////////////////////

//slist
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Slist;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class SlistBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class SlistMemberHook;

//list
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class T, class ...Options>
#endif
class List;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class ListBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class ListMemberHook;

//rbtree/set/multiset
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Rbtree;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Set;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Multiset;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class SetBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class SetMemberHook;

//splaytree/splay_set/splay_multiset
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Splaytree;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class SplaySet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class SplayMultiset;

//avltree/avl_set/avl_multiset
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Avltree;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class AvlSet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class AvlMultiset;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class AvlSetBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class AvlSetMemberHook;


//treap/treap_set/treap_multiset
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   >
#else
template<class T, class ...Options>
#endif
class Treap;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   >
#else
template<class T, class ...Options>
#endif
class TreapSet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   >
#else
template<class T, class ...Options>
#endif
class TreapMultiset;

//sgtree/sg_set/sg_multiset
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Sgtree;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class SgSet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class SgMultiset;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class Bstree;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class BsSet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   >
#else
template<class T, class ...Options>
#endif
class BsMultiset;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class BsSetBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class BsSetMemberHook;

//hashtable/unordered_set/unordered_multiset

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   , class O8  = void
   , class O9  = void
   , class O10 = void
   >
#else
template<class T, class ...Options>
#endif
class Hashtable;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   , class O8  = void
   , class O9  = void
   , class O10 = void
   , class O11 = void
   >
#else
template<class T, class ...Options>
#endif
class UnorderedSet;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class T
   , class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   , class O5  = void
   , class O6  = void
   , class O7  = void
   , class O8  = void
   , class O9  = void
   , class O10 = void
   , class O11 = void
   >
#else
template<class T, class ...Options>
#endif
class UnorderedMultiset;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class UnorderedSetBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   , class O4  = void
   >
#else
template<class ...Options>
#endif
class UnorderedSetMemberHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class AnyBaseHook;

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template
   < class O1  = void
   , class O2  = void
   , class O3  = void
   >
#else
template<class ...Options>
#endif
class AnyMemberHook;

//Options

template<bool Enabled>
struct ConstantTimeSize;

template<typename TSizeType>
struct SizeType;

template<typename TCompare>
struct Compare;

template<bool Enabled>
struct FloatingPoint;

template<typename TEqual>
struct Equal;

template<typename TPriority>
struct Priority;

template<typename THash>
struct Hash;

template<typename TValueTraits> struct ValueTraits;

template< typename Parent
        , typename TMemberHook
        , TMemberHook Parent::* PtrToMember>
struct MemberHook;

template<typename Functor>
struct FunctionHook;

template<typename TBaseHook>
struct BaseHook;

template<typename TVoidPointer>
struct VoidPointer;

template<typename TTag>
struct Tag;

template<LinkModeType LinkType>
struct LinkMode;

template<bool Enabled> struct
OptimizeSize;

template<bool Enabled>
struct Linear;

template<bool Enabled>
struct CacheLast;

template<typename TBucketTraits>
struct BucketTraits;

template<bool Enabled>
struct StoreHash;

template<bool Enabled>
struct OptimizeMultikey;

template<bool Enabled>
struct Power2Buckets;

template<bool Enabled>
struct CacheBegin;

template<bool Enabled>
struct CompareHash;

template<bool Enabled>
struct Incremental;

//Value traits

template<typename TValueTraits>
struct ValueTraits;

template< typename Parent
        , typename TMemberHook
        , TMemberHook Parent::* PtrToMember>
struct MemberHook;

template< typename Functor>
struct FunctionHook;

template<typename TBaseHook>
struct BaseHook;

template<class T, class NodeTraits, LinkModeType LinkMode = SafeLink>
struct DerivationValueTraits;

template<class NodeTraits, LinkModeType LinkMode = NormalLink>
struct TrivialValueTraits;

//Additional utilities

template<typename VoidPointer, std::size_t Alignment>
struct MaxPointerPlusBits;

template<std::size_t Alignment>
struct MaxPointerPlusBits<void *, Alignment>;

template<typename Pointer, std::size_t NumBits>
struct pointer_plus_bits;

template<typename T, std::size_t NumBits>
struct pointer_plus_bits<T *, NumBits>;

template<typename Ptr>
struct PointerTraits;

template<typename T>
struct PointerTraits<T *>;

}  //namespace intrusive {
}  //namespace boost {

#endif   //#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

#endif   //#ifndef BOOST_INTRUSIVE_FWD_HPP
