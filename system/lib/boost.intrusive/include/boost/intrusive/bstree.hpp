/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2013-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_BSTREE_HPP
#define BOOST_INTRUSIVE_BSTREE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/bs_set_hook.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/tree_iterator.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>
#include <boost/intrusive/detail/empty_node_checker.hpp>
#include <boost/intrusive/detail/default_header_holder.hpp>
#include <boost/intrusive/detail/reverse_iterator.hpp>
#include <boost/intrusive/detail/exception_disposer.hpp>
#include <boost/intrusive/detail/node_cloner_disposer.hpp>
#include <boost/intrusive/detail/key_nodeptr_comp.hpp>
#include <boost/intrusive/detail/simple_disposers.hpp>
#include <boost/intrusive/detail/size_holder.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/detail/algorithm.hpp>
#include <boost/intrusive/detail/tree_value_compare.hpp>

#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/parent_from_member.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/move/adl_move_swap.hpp>

#include <boost/intrusive/detail/minimal_pair_header.hpp>
#include <cstddef>   //size_t...
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>//less, equal_to

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

struct DefaultBstreeHookApplier
{  template <class T> struct Apply{ typedef typename T::default_bstree_hook Type;  };  };

template<>
struct IsDefaultHookTag<DefaultBstreeHookApplier>
{  static const bool VALUE = true;  };

struct BstreeDefaults
{
   typedef DefaultBstreeHookApplier ProtoValueTraits;
   static const bool CONSTANT_TIME_SIZE = true;
   typedef std::size_t SizeType;
   typedef void Compare;
   typedef void KeyOfValue;
   static const bool FLOATING_POINT = true;  //For sgtree
   typedef void Priority;  //For treap
   typedef void HeaderHolderType;
};

template<class TValueTraits, AlgoTypes AlgoType, typename HeaderHolder>
struct Bstbase3
{
   typedef TValueTraits                                               ValueTraits;
   typedef typename ValueTraits::NodeTraits                        NodeTraits;
   typedef typename NodeTraits::node                                NodeType;
   typedef typename get_algo<AlgoType, NodeTraits>::type            NodeAlgorithms;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef TreeIterator<ValueTraits, false>                                                   Iterator;
   typedef TreeIterator<ValueTraits, true>                                                    ConstIterator;
   typedef boost::intrusive::ReverseIterator<Iterator>                                         ReverseIterator;
   typedef boost::intrusive::ReverseIterator<ConstIterator>                                   ConstReverseIterator;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::Pointer)                               Pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::ConstPointer)                         ConstPointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<Pointer>::element_type)               ValueType;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<Pointer>::Reference)                  Reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::reference)            ConstReference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::DifferenceType)      DifferenceType;
   typedef typename detail::GetHeaderHolderType
      < ValueTraits,HeaderHolder >::type                                                       HeaderHolderType;

   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;
   static const bool STATEFUL_VALUE_TRAITS = detail::IsStatefulValueTraits<ValueTraits>::value;
   static const bool HAS_CONTAINER_FROM_ITERATOR =
        detail::is_same< HeaderHolderType, detail::DefaultHeaderHolder< NodeTraits > >::value;

   struct HolderT : public TValueTraits
   {
      inline explicit HolderT(const TValueTraits &vtraits)
         : TValueTraits(vtraits)
      {}
      HeaderHolderType m_root;
   } m_holder;

   static Bstbase3 &get_tree_base_from_end_iterator(const ConstIterator &end_iterator)
   {
      BOOST_INTRUSIVE_STATIC_ASSERT(HAS_CONTAINER_FROM_ITERATOR);
      NodePtr p = end_iterator.pointed_node();
      HeaderHolderType* h = HeaderHolderType::get_holder(p);
      HolderT *holder = get_parent_from_member<HolderT, HeaderHolderType>(h, &HolderT::root);
      Bstbase3 *base   = get_parent_from_member<Bstbase3, HolderT> (holder, &Bstbase3::m_holder);
      return *base;
   }

   inline Bstbase3(const TValueTraits &vtraits)
      : m_holder(vtraits)
   {
      NodeAlgorithms::init_header(this->header_ptr());
   }

   inline NodePtr header_ptr()
   { return m_holder.m_root.get_node(); }

   inline ConstNodePtr header_ptr() const
   { return m_holder.m_root.get_node(); }

   inline const ValueTraits &get_value_traits() const
   {  return this->m_holder;  }

   inline ValueTraits &get_value_traits()
   {  return this->m_holder;  }

   typedef typename boost::intrusive::ValueTraitsPointers
      <TValueTraits>::ConstValueTraitsPtr ConstValueTraitsPtr;

   inline ConstValueTraitsPtr priv_value_traits_ptr() const
   {  return PointerTraits<ConstValueTraitsPtr>::pointer_to(this->get_value_traits());  }

   inline Iterator begin() BOOST_NOEXCEPT
   {  return Iterator(NodeAlgorithms::begin_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline ConstIterator begin() const BOOST_NOEXCEPT
   {  return cbegin();   }

   inline ConstIterator cbegin() const BOOST_NOEXCEPT
   {  return ConstIterator(NodeAlgorithms::begin_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline Iterator end() BOOST_NOEXCEPT
   {  return Iterator(NodeAlgorithms::end_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline ConstIterator end() const BOOST_NOEXCEPT
   {  return cend();  }

   inline ConstIterator cend() const BOOST_NOEXCEPT
   {  return ConstIterator(NodeAlgorithms::end_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline Iterator root()
   {  return Iterator(NodeAlgorithms::root_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline ConstIterator root() const
   {  return croot();   }

   inline ConstIterator croot() const
   {  return ConstIterator(NodeAlgorithms::root_node(this->header_ptr()), this->priv_value_traits_ptr());   }

   inline ReverseIterator rbegin()
   {  return ReverseIterator(end());  }

   inline ConstReverseIterator rbegin() const
   {  return ConstReverseIterator(end());  }

   inline ConstReverseIterator crbegin() const
   {  return ConstReverseIterator(end());  }

   inline ReverseIterator rend()
   {  return ReverseIterator(begin());   }

   inline ConstReverseIterator rend() const
   {  return ConstReverseIterator(begin());   }

   inline ConstReverseIterator crend() const
   {  return ConstReverseIterator(begin());   }

   void replace_node(Iterator replace_this, Reference with_this)
   {
      NodeAlgorithms::replace_node( get_value_traits().to_node_ptr(*replace_this)
                                   , this->header_ptr()
                                   , get_value_traits().to_node_ptr(with_this));
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(replace_this.pointed_node());
   }

   inline void rebalance() BOOST_NOEXCEPT
   {  NodeAlgorithms::rebalance(this->header_ptr()); }

   Iterator rebalance_subtree(Iterator r) BOOST_NOEXCEPT
   {  return Iterator(NodeAlgorithms::rebalance_subtree(r.pointed_node()), this->priv_value_traits_ptr()); }

   static Iterator s_iterator_to(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      return Iterator (ValueTraits::to_node_ptr(value), ConstValueTraitsPtr());
   }

   static ConstIterator s_iterator_to(ConstReference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      return ConstIterator (ValueTraits::to_node_ptr(*PointerTraits<Pointer>::const_cast_from(PointerTraits<ConstPointer>::pointer_to(value))), ConstValueTraitsPtr());
   }

   Iterator iterator_to(Reference value) BOOST_NOEXCEPT
   {  return Iterator (this->get_value_traits().to_node_ptr(value), this->priv_value_traits_ptr()); }

   ConstIterator iterator_to(ConstReference value) const BOOST_NOEXCEPT
   {  return ConstIterator (this->get_value_traits().to_node_ptr(*PointerTraits<Pointer>::const_cast_from(PointerTraits<ConstPointer>::pointer_to(value))), this->priv_value_traits_ptr()); }

   inline static void init_node(Reference value)
   { NodeAlgorithms::init(ValueTraits::to_node_ptr(value)); }

};

template<class Less, class T>
struct GetCompare
{
   typedef Less Type;
};

template<class T>
struct GetCompare<void, T>
{
   typedef ::std::less<T> Type;
};

template<class KeyOfValue, class T>
struct GetKeyOfValue
{
   typedef KeyOfValue Type;
};

template<class T>
struct GetKeyOfValue<void, T>
{
   typedef ::boost::intrusive::detail::identity<T> Type;
};

template<class ValuePtr, class VoidOrKeyOfValue, class VoidOrKeyComp>
struct BstKeyTypes
{
   typedef typename
      boost::movelib::pointer_element<ValuePtr>::type ValueType;
   typedef typename GetKeyOfValue
      < VoidOrKeyOfValue, ValueType>::type           KeyOfValue;
   typedef typename KeyOfValue::type                KeyType;
   typedef typename GetCompare< VoidOrKeyComp
                      , KeyType
                      >::type                         KeyCompare;
   typedef TreeValueCompare
      <ValuePtr, KeyCompare, KeyOfValue>           ValueCompare;
};

template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, AlgoTypes AlgoType, typename HeaderHolder>
struct Bstbase2
   //Put the (possibly empty) functor in the first position to get EBO in MSVC
   //Use public inheritance to avoid MSVC bugs with closures
   : public detail::EboFunctorHolder
            < typename BstKeyTypes
               < typename TValueTraits::pointer
               , VoidOrKeyOfValue
               , VoidOrKeyComp
               
               >::value_compare
            >
   , public Bstbase3<TValueTraits, AlgoType, HeaderHolder>
{
   typedef Bstbase3<TValueTraits, AlgoType, HeaderHolder>             TreeheaderT;
   typedef BstKeyTypes< typename TValueTraits::pointer
                        , VoidOrKeyOfValue
                        , VoidOrKeyComp>                             KeyTypes;
   typedef typename TreeheaderT::ValueTraits                       ValueTraits;
   typedef typename TreeheaderT::NodeAlgorithms                    NodeAlgorithms;
   typedef typename TValueTraits::ValueType                          ValueType;
   typedef typename KeyTypes::KeyType                              KeyType;
   typedef typename KeyTypes::KeyOfValue                          KeyOfValue;
   typedef typename KeyTypes::KeyCompare                           KeyCompare;
   typedef typename KeyTypes::ValueCompare                         ValueCompare;
   typedef typename TreeheaderT::Iterator                           Iterator;
   typedef typename TreeheaderT::ConstIterator                     ConstIterator;
   typedef typename TreeheaderT::NodePtr                           NodePtr;
   typedef typename TreeheaderT::ConstNodePtr                     ConstNodePtr;

   Bstbase2(const KeyCompare &comp, const TValueTraits &vtraits)
      : detail::EboFunctorHolder<ValueCompare>(ValueCompare(comp)), TreeheaderT(vtraits)
   {}

   const ValueCompare &get_comp() const
   {  return this->get();  }

   ValueCompare &get_comp()
   {  return this->get();  }

   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::Pointer)                               Pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::ConstPointer)                         ConstPointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<Pointer>::Reference)                  Reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::reference)            ConstReference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::DifferenceType)      DifferenceType;
   typedef typename NodeAlgorithms::InsertCommitData InsertCommitData;

   inline ValueCompare value_comp() const
   {  return this->get_comp();   }

   inline KeyCompare key_comp() const
   {  return this->get_comp().key_comp();   }

   //lower_bound
   inline Iterator lower_bound(const KeyType &key)
   {  return this->lower_bound(key, this->key_comp());   }

   inline ConstIterator lower_bound(const KeyType &key) const
   {  return this->lower_bound(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   Iterator lower_bound(const KeyType &key, KeyTypeKeyCompare comp)
   {
      return Iterator(NodeAlgorithms::lower_bound
         (this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   template<class KeyType, class KeyTypeKeyCompare>
   ConstIterator lower_bound(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      return ConstIterator(NodeAlgorithms::lower_bound
         (this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   //upper_bound
   inline Iterator upper_bound(const KeyType &key)
   {  return this->upper_bound(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   Iterator upper_bound(const KeyType &key, KeyTypeKeyCompare comp)
   {
      return Iterator(NodeAlgorithms::upper_bound
         (this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   inline ConstIterator upper_bound(const KeyType &key) const
   {  return this->upper_bound(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   ConstIterator upper_bound(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      return ConstIterator(NodeAlgorithms::upper_bound
         (this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   template<class KeyTypeKeyCompare>
   struct KeyNodeCompRet
   {  typedef detail::KeyNodeptrComp<KeyTypeKeyCompare, ValueTraits, KeyOfValue> Type;  };

   template<class KeyTypeKeyCompare>
   inline typename KeyNodeCompRet<KeyTypeKeyCompare>::type key_node_comp(KeyTypeKeyCompare comp) const
   {
      return detail::KeyNodeptrComp<KeyTypeKeyCompare, ValueTraits, KeyOfValue>(comp, &this->get_value_traits());
   }

   //find
   inline Iterator find(const KeyType &key)
   {  return this->find(key, this->key_comp()); }

   template<class KeyType, class KeyTypeKeyCompare>
   Iterator find(const KeyType &key, KeyTypeKeyCompare comp)
   {
      return Iterator
         (NodeAlgorithms::find(this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   inline ConstIterator find(const KeyType &key) const
   {  return this->find(key, this->key_comp()); }

   template<class KeyType, class KeyTypeKeyCompare>
   ConstIterator find(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      return ConstIterator
         (NodeAlgorithms::find(this->header_ptr(), key, this->key_node_comp(comp)), this->priv_value_traits_ptr());
   }

   //equal_range
   inline std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->equal_range(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> equal_range(const KeyType &key, KeyTypeKeyCompare comp)
   {
      std::pair<NodePtr, NodePtr> ret = 
         NodeAlgorithms::equal_range(this->header_ptr(), key, this->key_node_comp(comp));
      return std::pair<Iterator, Iterator>( Iterator(ret.first, this->priv_value_traits_ptr())
                                          , Iterator(ret.second, this->priv_value_traits_ptr()));
   }

   inline std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->equal_range(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      std::pair<NodePtr, NodePtr> ret =
         NodeAlgorithms::equal_range(this->header_ptr(), key, this->key_node_comp(comp));
      return std::pair<ConstIterator, ConstIterator>( ConstIterator(ret.first, this->priv_value_traits_ptr())
                                                      , ConstIterator(ret.second, this->priv_value_traits_ptr()));
   }

   //lower_bound_range
   inline std::pair<Iterator,Iterator> lower_bound_range(const KeyType &key)
   {  return this->lower_bound_range(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> lower_bound_range(const KeyType &key, KeyTypeKeyCompare comp)
   {
      std::pair<NodePtr, NodePtr> ret =
         NodeAlgorithms::lower_bound_range(this->header_ptr(), key, this->key_node_comp(comp));
      return std::pair<Iterator, Iterator>( Iterator(ret.first, this->priv_value_traits_ptr())
                                          , Iterator(ret.second, this->priv_value_traits_ptr()));
   }

   inline std::pair<ConstIterator, ConstIterator>
      lower_bound_range(const KeyType &key) const
   {  return this->lower_bound_range(key, this->key_comp());   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator, ConstIterator>
      lower_bound_range(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      std::pair<NodePtr, NodePtr> ret =
         NodeAlgorithms::lower_bound_range(this->header_ptr(), key, this->key_node_comp(comp));
      return std::pair<ConstIterator, ConstIterator>( ConstIterator(ret.first, this->priv_value_traits_ptr())
                                                      , ConstIterator(ret.second, this->priv_value_traits_ptr()));
   }

   //bounded_range
   inline std::pair<Iterator,Iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, bool left_closed, bool right_closed)
   {  return this->bounded_range(lower_key, upper_key, this->key_comp(), left_closed, right_closed);   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed)
   {
      std::pair<NodePtr, NodePtr> ret =
         NodeAlgorithms::bounded_range
            (this->header_ptr(), lower_key, upper_key, this->key_node_comp(comp), left_closed, right_closed);
      return std::pair<Iterator, Iterator>( Iterator(ret.first, this->priv_value_traits_ptr())
                                          , Iterator(ret.second, this->priv_value_traits_ptr()));
   }

   inline std::pair<ConstIterator,ConstIterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, bool left_closed, bool right_closed) const
   {  return this->bounded_range(lower_key, upper_key, this->key_comp(), left_closed, right_closed);   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator,ConstIterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const
   {
      std::pair<NodePtr, NodePtr> ret =
         NodeAlgorithms::bounded_range
            (this->header_ptr(), lower_key, upper_key, this->key_node_comp(comp), left_closed, right_closed);
      return std::pair<ConstIterator, ConstIterator>( ConstIterator(ret.first, this->priv_value_traits_ptr())
                                                      , ConstIterator(ret.second, this->priv_value_traits_ptr()));
   }

   //insert_unique_check
   inline std::pair<Iterator, bool> insert_unique_check
      (const KeyType &key, InsertCommitData &commit_data)
   {  return this->insert_unique_check(key, this->key_comp(), commit_data);   }

   inline std::pair<Iterator, bool> insert_unique_check
      (ConstIterator hint, const KeyType &key, InsertCommitData &commit_data)
   {  return this->insert_unique_check(hint, key, this->key_comp(), commit_data);   }

   template<class KeyType, class KeyTypeKeyCompare>
   BOOST_INTRUSIVE_DOC1ST(std::pair<iterator BOOST_INTRUSIVE_I bool>
      , typename detail::disable_if_convertible
         <KeyType BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I 
         std::pair<Iterator BOOST_INTRUSIVE_I bool> >::type)
      insert_unique_check
      (const KeyType &key, KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         (NodeAlgorithms::insert_unique_check
            (this->header_ptr(), key, this->key_node_comp(comp), commit_data));
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }

   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_unique_check
      (ConstIterator hint, const KeyType &key, KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         (NodeAlgorithms::insert_unique_check
            (this->header_ptr(), hint.pointed_node(), key, this->key_node_comp(comp), commit_data));
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }
};

//Due to MSVC's EBO implementation, to save space and maintain the ABI, we must put the non-empty size member
//in the first position, but if size is not going to be stored then we'll use an specialization
//that doesn't inherit from size_holder
template<class ValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, bool ConstantTimeSize, class TSizeType, AlgoTypes VAlgoType, typename HeaderHolder>
struct BstbaseHack
   : public detail::SizeHolder<ConstantTimeSize, TSizeType>
   , public Bstbase2 < ValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VAlgoType, HeaderHolder>
{
   typedef Bstbase2< ValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VAlgoType, HeaderHolder> BaseType;
   typedef typename BaseType::KeyCompare         KeyCompare;
   typedef typename BaseType::ValueCompare       ValueCompare;
   typedef TSizeType                                SizeType;
   typedef typename BaseType::NodeTraits         NodeTraits;
   typedef typename get_algo
      <VAlgoType, NodeTraits>::type                AlgoType;

   inline BstbaseHack(const KeyCompare & comp, const ValueTraits &vtraits)
      : BaseType(comp, vtraits)
   {
      this->sz_traits().set_size(SizeType(0));
   }

   typedef detail::SizeHolder<ConstantTimeSize, TSizeType>     SizeTraits;

   inline SizeTraits &sz_traits()
   {  return static_cast<SizeTraits &>(*this);  }

   inline const SizeTraits &sz_traits() const
   {  return static_cast<const SizeTraits &>(*this);  }
};

//Specialization for ConstantTimeSize == false
template<class ValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class SizeType, AlgoTypes AlgoType, typename HeaderHolder>
struct BstbaseHack<ValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, false, SizeType, AlgoType, HeaderHolder>
   : public Bstbase2 < ValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, AlgoType, HeaderHolder>
{
   typedef Bstbase2< ValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, AlgoType, HeaderHolder> BaseType;
   typedef typename BaseType::ValueCompare       ValueCompare;
   typedef typename BaseType::KeyCompare         KeyCompare;
   inline BstbaseHack(const KeyCompare & comp, const ValueTraits &vtraits)
      : BaseType(comp, vtraits)
   {}

   typedef detail::SizeHolder<false, SizeType>     SizeTraits;

   inline SizeTraits sz_traits() const
   {  return SizeTraits();  }
};

//This class will
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, bool ConstantTimeSize, class TSizeType, AlgoTypes AlgoType, typename HeaderHolder>
struct Bstbase
   : public BstbaseHack< TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, ConstantTimeSize, TSizeType, AlgoType, HeaderHolder>
{
   typedef BstbaseHack< TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, ConstantTimeSize, TSizeType, AlgoType, HeaderHolder> BaseType;
   typedef TValueTraits                             ValueTraits;
   typedef typename BaseType::ValueCompare       ValueCompare;
   typedef typename BaseType::KeyCompare         KeyCompare;
   typedef typename BaseType::ConstReference     ConstReference;
   typedef typename BaseType::Reference           Reference;
   typedef typename BaseType::Iterator            Iterator;
   typedef typename BaseType::ConstIterator      ConstIterator;
   typedef typename BaseType::NodeTraits         NodeTraits;
   typedef typename get_algo
      <AlgoType, NodeTraits>::type                NodeAlgorithms;
   typedef TSizeType                                SizeType;

   inline Bstbase(const KeyCompare & comp, const TValueTraits &vtraits)
      : BaseType(comp, vtraits)
   {}

   //Detach all inserted nodes. This will add exception safety to bstree_impl
   //constructors inserting elements.
   ~Bstbase()
   #if defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
      requires (TValueTraits::link_mode != NormalLink)
   #endif
   {
      BOOST_IF_CONSTEXPR(IsSafeAutounlink<ValueTraits::link_mode>::VALUE){
         NodeAlgorithms::clear_and_dispose
            ( this->header_ptr()
            , detail::NodeDisposer<detail::NullDisposer, ValueTraits, AlgoType>
               (detail::NullDisposer(), &this->get_value_traits()));
         NodeAlgorithms::init(this->header_ptr());
      }
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) && defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
   //Default destructor for normal links (allows conditional triviality)
   ~Bstbase() requires (TValueTraits::link_mode == NormalLink) = default;
   #endif
};


/// @endcond

//! The class template bstree is an unbalanced intrusive binary search tree
//! container. The no-throw guarantee holds only, if the key_compare object
//! doesn't throw.
//!
//! The complexity guarantees only hold if the tree is balanced, logarithmic
//! complexity would increase to linear if the tree is totally unbalanced.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class TSizeType, bool ConstantTimeSize, AlgoTypes VAlgoType, typename HeaderHolder>
#endif
class BstreeImpl
   :  public Bstbase<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, ConstantTimeSize, TSizeType, VAlgoType, HeaderHolder>
{
   public:
   /// @cond
   typedef Bstbase<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, ConstantTimeSize, TSizeType, VAlgoType, HeaderHolder> DataType;
   typedef TreeIterator<TValueTraits, false> IteratorType;
   typedef TreeIterator<TValueTraits, true>  ConstIteratorType;
   /// @endcond

   typedef BOOST_INTRUSIVE_IMPDEF(TValueTraits)                                                  ValueTraits;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::Pointer)                               Pointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::ConstPointer)                         ConstPointer;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<Pointer>::element_type)               ValueType;
   typedef BOOST_INTRUSIVE_IMPDEF(typename DataType::KeyType)                                 KeyType;
   typedef BOOST_INTRUSIVE_IMPDEF(typename DataType::KeyOfValue)                             KeyOfValue;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<Pointer>::Reference)                  Reference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::reference)            ConstReference;
   typedef BOOST_INTRUSIVE_IMPDEF(typename PointerTraits<ConstPointer>::DifferenceType)      DifferenceType;
   typedef BOOST_INTRUSIVE_IMPDEF(TSizeType)                                                     SizeType;
   typedef BOOST_INTRUSIVE_IMPDEF(typename DataType::ValueCompare)                            ValueCompare;
   typedef BOOST_INTRUSIVE_IMPDEF(typename DataType::KeyCompare)                              KeyCompare;
   typedef BOOST_INTRUSIVE_IMPDEF(IteratorType)                                                Iterator;
   typedef BOOST_INTRUSIVE_IMPDEF(ConstIteratorType)                                          ConstIterator;
   typedef BOOST_INTRUSIVE_IMPDEF(boost::intrusive::ReverseIterator<Iterator>)                 ReverseIterator;
   typedef BOOST_INTRUSIVE_IMPDEF(boost::intrusive::ReverseIterator<ConstIterator>)           ConstReverseIterator;
   typedef BOOST_INTRUSIVE_IMPDEF(typename ValueTraits::NodeTraits)                           NodeTraits;
   typedef BOOST_INTRUSIVE_IMPDEF(typename NodeTraits::Node)                                   Node;
   typedef BOOST_INTRUSIVE_IMPDEF(typename NodeTraits::NodePtr)                               NodePtr;
   typedef BOOST_INTRUSIVE_IMPDEF(typename NodeTraits::ConstNodePtr)                         ConstNodePtr;
   /// @cond
   typedef typename get_algo<VAlgoType, NodeTraits>::type                                       AlgoType;
   /// @endcond
   typedef BOOST_INTRUSIVE_IMPDEF(AlgoType)                                                    NodeAlgorithms;

   static const bool CONSTANT_TIME_SIZE = ConstantTimeSize;
   static const bool STATEFUL_VALUE_TRAITS = detail::IsStatefulValueTraits<ValueTraits>::value;
   /// @cond
   private:

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BstreeImpl)

   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CONSTANT_TIME_SIZE && ((int)ValueTraits::link_mode == (int)AutoUnlink)));


   protected:


   /// @endcond

   public:

   typedef typename NodeAlgorithms::InsertCommitData InsertCommitData;

   //! <b>Effects</b>: Constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the key_compare object throws. Basic guarantee.
   BstreeImpl()
      :  DataType(KeyCompare(), ValueTraits())
   {}

   //! <b>Effects</b>: Constructs an empty container with given comparison and traits.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the key_compare object throws. Basic guarantee.
   explicit BstreeImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  DataType(cmp, v_traits)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty container and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is the distance between first and last.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the key_compare object throws. Basic guarantee.
   template<class Iterator>
   BstreeImpl( bool unique, Iterator b, Iterator e
              , const KeyCompare &cmp     = KeyCompare()
              , const ValueTraits &v_traits = ValueTraits())
      : DataType(cmp, v_traits)
   {
      //bstbase releases elements in case of exceptions
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! <b>Effects</b>: Constructs a container moving resources from another container.
   //!   Internal comparison object and value traits are move constructed and
   //!   nodes belonging to x (except the node representing the "end") are linked to *this.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node's
   //!   move constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the move constructor of the comparison objet throws.
   BstreeImpl(BOOST_RV_REF(BstreeImpl) x)
      : DataType(::boost::move(x.get_comp()), ::boost::move(x.get_value_traits()))
   {
      this->swap(x);
   }

   //! <b>Effects</b>: Equivalent to swap
   //!
   inline BstreeImpl& operator=(BOOST_RV_REF(BstreeImpl) x)
   {  this->swap(x); return *this;  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! <b>Effects</b>: Detaches all elements from this. The objects in the set
   //!   are not deleted (i.e. no destructors are called), but the nodes according to
   //!   the value_traits template parameter are reinitialized and thus can be reused.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this.
   //!
   //! <b>Throws</b>: Nothing.
   ~BstreeImpl()
   {}

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator begin() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns an iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator end() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator end() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning of the
   //!    reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //!    of the reversed container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a iterator pointing to the root node of the container or end() if not present.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator root() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the root node of the container or cend() if not present.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator root() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the root node of the container or cend() if not present.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator croot() const BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   static BstreeImpl& container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {
      return static_cast<BstreeImpl&>
               (DataType::get_tree_base_from_end_iterator(end_iterator));
   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   static const BstreeImpl & container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {
      return static_cast<BstreeImpl&>
               (DataType::get_tree_base_from_end_iterator(end_iterator));
   }

   //! <b>Precondition</b>: it must be a valid iterator
   //!   of the container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic.
   BOOST_INTRUSIVE_NO_DANGLING
   static BstreeImpl & container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return container_from_end_iterator(it.end_iterator_from_it());   }

   //! <b>Precondition</b>: it must be a valid end const_iterator
   //!   of container.
   //!
   //! <b>Effects</b>: Returns a const reference to the container associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic.
   BOOST_INTRUSIVE_NO_DANGLING
   static const BstreeImpl & container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return container_from_end_iterator(it.end_iterator_from_it());   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns the key_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If key_compare copy-constructor throws.
   key_compare key_comp() const;

   //! <b>Effects</b>: Returns the value_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_compare copy-constructor throws.
   value_compare value_comp() const;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns true if the container is empty.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   [[nodiscard]] bool empty() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
         return !this->DataType::sz_traits().get_size();
      }
      else{
         return AlgoType::unique(this->header_ptr());
      }
   }

   //! <b>Effects</b>: Returns the number of elements stored in the container.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this
   //!   if constant-time size option is disabled. Constant time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   SizeType size() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         return this->sz_traits().get_size();
      else{
         return (SizeType)NodeAlgorithms::size(this->header_ptr());
      }
   }

   //! <b>Effects</b>: Swaps the contents of two containers.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the comparison functor's swap call throws.
   void swap(BstreeImpl& other)
   {
      //This can throw
      ::boost::adl_move_swap(this->get_comp(), other.get_comp());
      //These can't throw
      NodeAlgorithms::swap_tree(this->header_ptr(), NodePtr(other.header_ptr()));
      this->sz_traits().swap(other.sz_traits());
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const BstreeImpl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!src.empty()){
         detail::ExceptionDisposer<BstreeImpl, Disposer>
            rollback(*this, disposer);
         NodeAlgorithms::clone
            (src.header_ptr()
            ,this->header_ptr()
            ,detail::NodeCloner <Cloner,    ValueTraits, VAlgoType>(cloner,   &this->get_value_traits())
            ,detail::NodeDisposer<Disposer, ValueTraits, VAlgoType>(disposer, &this->get_value_traits()));
         this->sz_traits().set_size(src.sz_traits().get_size());
         this->get_comp() = src.get_comp();
         rollback.release();
      }
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(reference)
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   //!
   //! <b>Note</b>: This version can modify the source container, useful to implement
   //!    move semantics.
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(BstreeImpl) src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!src.empty()){
         detail::ExceptionDisposer<BstreeImpl, Disposer>
            rollback(*this, disposer);
         NodeAlgorithms::clone
            (src.header_ptr()
            ,this->header_ptr()
            ,detail::NodeCloner <Cloner,    ValueTraits, VAlgoType, false>(cloner,   &this->get_value_traits())
            ,detail::NodeDisposer<Disposer, ValueTraits, VAlgoType>(disposer, &this->get_value_traits()));
         this->sz_traits().set_size(src.sz_traits().get_size());
         this->get_comp() = src.get_comp();
         rollback.release();
      }
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container before the upper bound.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal key_compare ordering function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_equal(Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      Iterator ret(NodeAlgorithms::insert_equal_upper_bound
         (this->header_ptr(), to_insert, this->key_node_comp(this->key_comp())), this->priv_value_traits_ptr());
      this->sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator.
   //!
   //! <b>Effects</b>: Inserts x into the container, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case)
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the internal key_compare ordering function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_equal(ConstIterator hint, Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      Iterator ret(NodeAlgorithms::insert_equal
         (this->header_ptr(), hint.pointed_node(), to_insert, this->key_node_comp(this->key_comp())), this->priv_value_traits_ptr());
      this->sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Inserts a each element of a range into the container
   //!   before the upper bound of the key of each element.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //!
   //! <b>Throws</b>: If the comparison functor call throws.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      Iterator iend(this->end());
      for (; b != e; ++b)
         this->insert_equal(iend, *b);
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container if the value
   //!   is not already present.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the comparison functor call throws.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<Iterator, bool> insert_unique(Reference value)
   {
      InsertCommitData commit_data;
      std::pair<NodePtr, bool> ret =
         (NodeAlgorithms::insert_unique_check
            (this->header_ptr(), KeyOfValue()(value), this->key_node_comp(this->key_comp()), commit_data));
      return std::pair<Iterator, bool>
         ( ret.second ? this->insert_unique_commit(value, commit_data)
                      : Iterator(ret.first, this->priv_value_traits_ptr())
         , ret.second);
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator
   //!
   //! <b>Effects</b>: Tries to insert x into the container, using "hint" as a hint
   //!   to where it will be inserted.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time (two comparisons in the worst case)
   //!   if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the comparison functor call throws.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_unique(ConstIterator hint, Reference value)
   {
      InsertCommitData commit_data;
      std::pair<NodePtr, bool> ret =
         (NodeAlgorithms::insert_unique_check
            (this->header_ptr(), hint.pointed_node(), KeyOfValue()(value), this->key_node_comp(this->key_comp()), commit_data));
      return ret.second ? this->insert_unique_commit(value, commit_data)
                        : Iterator(ret.first, this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Tries to insert each element of a range into the container.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by value_comp().
   //!
   //! <b>Throws</b>: If the comparison functor call throws.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      if(this->empty()){
         Iterator iend(this->end());
         for (; b != e; ++b)
            this->insert_unique(iend, *b);
      }
      else{
         for (; b != e; ++b)
            this->insert_unique(*b);
      }
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the comp ordering function throws. Strong guarantee.
   std::pair<iterator, bool> insert_unique_check(const key_type &key, insert_commit_data &commit_data);

   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint"
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the comp ordering function throws. Strong guarantee.
   std::pair<iterator, bool> insert_unique_check(const_iterator hint, const key_type &key, insert_commit_data &commit_data);

   //! <b>Requires</b>: comp must be a comparison function that induces
   //!   the same strict weak ordering as key_compare. The difference is that
   //!   comp compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the comp ordering function throws. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that
   //!   part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyTypeKeyCompare comp, insert_commit_data &commit_data);

   //! <b>Requires</b>: comp must be a comparison function that induces
   //!   the same strict weak ordering as key_compare. The difference is that
   //!   comp compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint"
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the comp ordering function throws. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   constructing that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that key
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This can give a total
   //!   constant-time complexity to the insertion: check(O(1)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyTypeKeyCompare comp, insert_commit_data &commit_data);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the container between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //! <b>Effects</b>: Inserts the value in the container using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   Iterator insert_unique_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));

      #if !(defined(BOOST_DISABLE_ASSERTS) || ( defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && defined(NDEBUG) ))
      //Test insertion position is correct
      Iterator p(commit_data.node, this->priv_value_traits_ptr());
      if(!commit_data.link_left){
         ++p;
      }
      //Check if the insertion point is correct to detect wrong
      //uses insert_unique_check
      BOOST_ASSERT(( p == this->end()   || !this->get_comp()(*p, value)   ));
      BOOST_ASSERT(( p == this->begin() || !this->get_comp()(value, *--p) ));
      #endif

      NodeAlgorithms::insert_unique_commit
               (this->header_ptr(), to_insert, commit_data);
      this->sz_traits().increment();
      return Iterator(to_insert, this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate
   //!
   //! <b>Effects</b>: Inserts x into the container before "pos".
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" container ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   Iterator insert_before(ConstIterator pos, Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      this->sz_traits().increment();
      return Iterator(NodeAlgorithms::insert_before
         (this->header_ptr(), pos.pointed_node(), to_insert), this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the last position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      this->sz_traits().increment();
      NodeAlgorithms::push_back(this->header_ptr(), to_insert);
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the first position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      this->sz_traits().increment();
      NodeAlgorithms::push_front(this->header_ptr(), to_insert);
   }

   //! <b>Effects</b>: Erases the element pointed to by i.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   Iterator erase(ConstIterator i) BOOST_NOEXCEPT
   {
      ConstIterator ret(i);
      ++ret;
      NodePtr to_erase(i.pointed_node());
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(to_erase));
      NodeAlgorithms::erase(this->header_ptr(), to_erase);
      this->sz_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      return ret.unconst();
   }

   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   Iterator erase(ConstIterator b, ConstIterator e) BOOST_NOEXCEPT
   {  SizeType n;   return this->private_erase(b, e, n);   }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   SizeType erase(const KeyType &key) BOOST_NOEXCEPT
   {  return this->erase(key, this->key_comp());   }

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   comp(nk, key) and !comp(key, nk), with comp(nk, key) implying !comp(key, nk),
   //!   with nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyTypeKeyCompare>
   BOOST_INTRUSIVE_DOC1ST(size_type
      , typename detail::disable_if_convertible<KeyTypeKeyCompare BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I SizeType>::type)
      erase(const KeyType& key, KeyTypeKeyCompare comp)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key, comp);
      SizeType n;
      this->private_erase(p.first, p.second, n);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by i.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator i, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr to_erase(i.pointed_node());
      Iterator ret(this->erase(i));
      disposer(this->get_value_traits().to_value_ptr(to_erase));
      return ret;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   SizeType erase_and_dispose(const KeyType &key, Disposer disposer)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key);
      SizeType n;
      this->private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator b, ConstIterator e, Disposer disposer) BOOST_NOEXCEPT
   {  SizeType n;   return this->private_erase(b, e, n, disposer);   }

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   comp(nk, key) and !comp(key, nk), with comp(nk, key) implying !comp(key, nk)
   //!   and nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   BOOST_INTRUSIVE_DOC1ST(size_type
      , typename detail::disable_if_convertible<KeyTypeKeyCompare BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I SizeType>::type)
      erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key, comp);
      SizeType n;
      this->private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Effects</b>: Erases all of the elements.
   //!
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear() BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
         this->clear_and_dispose(detail::NullDisposer());
      }
      else{
         NodeAlgorithms::init_header(this->header_ptr());
         this->sz_traits().set_size(0);
      }
   }

   //! <b>Effects</b>: Erases all of the elements calling disposer(p) for
   //!   each node to be erased.
   //! <b>Complexity</b>: Average complexity for is at most O(log(size() + N)),
   //!   where N is the number of elements in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. Calls N times to disposer functor.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      NodeAlgorithms::clear_and_dispose(this->header_ptr()
         , detail::NodeDisposer<Disposer, ValueTraits, VAlgoType>(disposer, &this->get_value_traits()));
      NodeAlgorithms::init_header(this->header_ptr());
      this->sz_traits().set_size(0);
   }

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //!
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given value.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   SizeType count(const KeyType &key) const
   {  return SizeType(this->count(key, this->key_comp()));   }

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   comp(nk, key) and !comp(key, nk), with comp(nk, key) implying !comp(key, nk),
   //!   and nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //!
   //! <b>Complexity</b>: Logarithmic to the number of elements contained plus lineal
   //!   to number of objects with the given key.
   //!
   //! <b>Throws</b>: If `comp` throws.
   template<class KeyType, class KeyTypeKeyCompare>
   SizeType count(const KeyType &key, KeyTypeKeyCompare comp) const
   {
      std::pair<ConstIterator, ConstIterator> ret = this->equal_range(key, comp);
      SizeType n = 0;
      for(; ret.first != ret.second; ++ret.first){ ++n; }
      return n;
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //Add non-const overloads to theoretically const members
   //as some algorithms have different behavior when non-const versions are used (like splay trees).
   SizeType count(const KeyType &key)
   {  return SizeType(this->count(key, this->key_comp()));   }

   template<class KeyType, class KeyTypeKeyCompare>
   SizeType count(const KeyType &key, KeyTypeKeyCompare comp)
   {
      std::pair<ConstIterator, ConstIterator> ret = this->equal_range(key, comp);
      SizeType n = 0;
      for(; ret.first != ret.second; ++ret.first){ ++n; }
      return n;
   }

   #else //defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   iterator lower_bound(const key_type &key);

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is not less than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const key_type &)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType &key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType &key, KeyTypeKeyCompare comp) const;

   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   iterator upper_bound(const key_type &key);

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   !comp(key, nk), with nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Effects</b>: Returns an iterator to the first element whose
   //!   key is greater than k according to comp or end() if that element
   //!   does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `comp` throws.
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType &key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &)
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType &key, KeyTypeKeyCompare comp) const;

   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   iterator find(const key_type &key);

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   comp(nk, key) and !comp(key, nk), with comp(nk, key) implying !comp(key, nk),
   //!   and nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   k or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `comp` throws.
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType &key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::find(const key_type &)
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType &key, KeyTypeKeyCompare comp) const;

   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   std::pair<iterator,iterator> equal_range(const key_type &key);

   //! <b>Requires</b>: key is a value such that `*this` is partitioned with respect to
   //!   comp(nk, key) and !comp(key, nk), with comp(nk, key) implying !comp(key, nk),
   //!   with nk the key_type of a value_type inserted into `*this`.
   //!
   //! <b>Effects</b>: Finds a range containing all elements whose key is k or
   //!   an empty range that indicates the position where those elements would be
   //!   if they there is no elements with key k.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `comp` throws.
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> equal_range(const KeyType &key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const key_type &)
   std::pair<const_iterator, const_iterator> equal_range(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType &key, KeyTypeKeyCompare comp) const;

   //! <b>Requires</b>:
   //!   `upper_key` shall not precede `lower_key` according to key_compare.
   //!   [key_comp()(upper_key, lower_key) shall be false]
   //!
   //!   If `lower_key` is equivalent to `upper_key`
   //!   [!key_comp()(upper_key, lower_key) && !key_comp()(lower_key, upper_key)] then
   //!   ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key) if left_closed, upper_bound(lower_key) otherwise
   //!
   //!   second = upper_bound(upper_key) if right_closed, lower_bound(upper_key) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `key_compare` throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_value and upper_value.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   std::pair<iterator,iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_value, bool left_closed, bool right_closed);

   //! <b>Requires</b>:
   //!   `lower_key` is a value such that `*this` is partitioned with respect to
   //!   comp(nk, lower_key) if left_closed is true, with respect to !comp(lower_key, nk) otherwise.
   //!
   //! `upper_key` is a value such that `*this` is partitioned with respect to
   //!   !comp(upper_key, nk) if right_closed is true, with respect to comp(nk, upper_key) otherwise.
   //!
   //!   `upper_key` shall not precede `lower_key` according to comp
   //!   [comp(upper_key, lower_key) shall be false]
   //!
   //!   If `lower_key` is equivalent to `upper_key`
   //!   [!comp(upper_key, lower_key) && !comp(lower_key, upper_key)] then
   //!   ('left_closed' || 'right_closed') must be false.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key, comp) if left_closed, upper_bound(lower_key, comp) otherwise
   //!
   //!   second = upper_bound(upper_key, comp) if right_closed, lower_bound(upper_key, comp) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If `comp` throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_key and upper_key.
   //!
   //! <b>Note</b>: Experimental function, the interface might change in future releases.
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);
   
   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<const_iterator,const_iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator,const_iterator> bounded_range
      (const KeyType &lower_key, const KeyType &upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid iterator i belonging to the set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_iterator i belonging to the
   //!   set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! <b>Requires</b>: value shall not be in a container.
   //!
   //! <b>Effects</b>: init_node puts the hook of a value in a well-known default
   //!   state.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: This function puts the hook in the well-known default state
   //!   used by AutoUnlink and safe hooks.
   static void init_node(reference value) BOOST_NOEXCEPT;

   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Unlinks the leftmost node from the container.
   //!
   //! <b>Complexity</b>: Average complexity is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function breaks the container and the container can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the container.
   Pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT
   {
      NodePtr to_be_disposed(NodeAlgorithms::unlink_leftmost_without_rebalance
                           (this->header_ptr()));
      if(!to_be_disposed)
         return 0;
      this->sz_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)//If this is commented does not work with NormalLink
         NodeAlgorithms::init(to_be_disposed);
      return this->get_value_traits().to_value_ptr(to_be_disposed);
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Requires</b>: replace_this must be a valid iterator of *this
   //!   and with_this must not be inserted in any container.
   //!
   //! <b>Effects</b>: Replaces replace_this in its position in the
   //!   container with with_this. The container does not need to be rebalanced.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   with_this is not equivalent to *replace_this according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed.
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! <b>Effects</b>: Rebalances the tree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear.
   void rebalance() BOOST_NOEXCEPT;

   //! <b>Requires</b>: old_root is a node of a tree.
   //!
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   iterator rebalance_subtree(iterator r) BOOST_NOEXCEPT;

   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: removes "value" from the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Logarithmic time.
   //!
   //! <b>Note</b>: This static function is only usable with non-constant
   //! time size containers that have stateless comparison functors.
   //!
   //! If the user calls
   //! this function with a constant time size container or stateful comparison
   //! functor a compilation error will be issued.
   static void remove_node(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!CONSTANT_TIME_SIZE));
      NodePtr to_remove(ValueTraits::to_node_ptr(value));
      NodeAlgorithms::unlink(to_remove);
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_remove);
   }

   //! <b>Requires</b>: "source" container's Options can only can differ in the comparison
   //!   function from *this.
   //! 
   //! <b>Effects</b>: Attempts to extract each element in source and insert it into a using
   //!   the comparison object of *this. If there is an element in a with key equivalent to the
   //!   key of an element from source, then that element is not extracted from source.
   //! 
   //! <b>Postcondition</b>: Pointers and references to the transferred elements of source refer
   //!   to those same elements but as members of *this. Iterators referring to the transferred
   //!   elements will continue to refer to their elements, but they now behave as iterators into *this,
   //!   not into source.
   //!
   //! <b>Throws</b>: Nothing unless the comparison object throws.
   //!
   //! <b>Complexity</b>: N log(a.size() + N) (N has the value source.size())
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class T, class ...Options2> void merge_unique(bstree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_unique(BstreeImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, VAlgoType, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);
         if( NodeAlgorithms::transfer_unique(this->header_ptr(), this->key_node_comp(this->key_comp()), source.header_ptr(), p) ){
            source.sz_traits().decrement();
            this->sz_traits().increment();
         }
      }
   }

   //! <b>Requires</b>: "source" container's Options can only can differ in the comparison
   //!   function from *this.
   //! 
   //! <b>Effects</b>: Extracts each element in source and insert it into a using
   //!   the comparison object of *this.
   //! 
   //! <b>Postcondition</b>: Pointers and references to the transferred elements of source refer
   //!   to those same elements but as members of *this. Iterators referring to the transferred
   //!   elements will continue to refer to their elements, but they now behave as iterators into *this,
   //!   not into source.
   //!
   //! <b>Throws</b>: Nothing unless the comparison object throws.
   //!
   //! <b>Complexity</b>: N log(a.size() + N) (N has the value source.size())
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class T, class ...Options2> void merge_equal(bstree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_equal(BstreeImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, VAlgoType, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);
         NodeAlgorithms::transfer_equal(this->header_ptr(), this->key_node_comp(this->key_comp()), source.header_ptr(), p);
         source.sz_traits().decrement();
         this->sz_traits().increment();
      }
   }

   //! <b>Effects</b>: Asserts the integrity of the container with additional checks provided by the user.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Note</b>: The method might not have effect when asserts are turned off (e.g., with NDEBUG).
   //!   Experimental function, interface might change in future versions.
   template <class ExtraChecker>
   void check(ExtraChecker extra_checker) const
   {
      typedef detail::KeyNodeptrComp<KeyCompare, ValueTraits, KeyOfValue> NodeptrCompT;
      NodeptrCompT nodeptr_comp(this->key_comp(), &this->get_value_traits());
      typedef typename get_node_checker<VAlgoType, TValueTraits, NodeptrCompT, ExtraChecker>::type NodeCheckerT;
      typename NodeCheckerT::return_type checker_return;
      NodeAlgorithms::check(this->header_ptr(), NodeCheckerT(nodeptr_comp, extra_checker), checker_return);
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!CONSTANT_TIME_SIZE || this->sz_traits().get_size() == checker_return.node_count);
   }

   //! <b>Effects</b>: Asserts the integrity of the container.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Note</b>: The method has no effect when asserts are turned off (e.g., with NDEBUG).
   //!   Experimental function, interface might change in future versions.
   void check() const
   {
      check(detail::EmptyNodeChecker<TValueTraits>());
   }

   friend bool operator==(const BstreeImpl &x, const BstreeImpl &y)
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
      if(x.size() != y.size()){
         return false;
      }
      return boost::intrusive::algo_equal(x.cbegin(), x.cend(), y.cbegin(), y.cend());
   }

   friend bool operator!=(const BstreeImpl &x, const BstreeImpl &y)
   {  return !(x == y); }

   friend bool operator<(const BstreeImpl &x, const BstreeImpl &y)
   {  return ::boost::intrusive::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   friend bool operator>(const BstreeImpl &x, const BstreeImpl &y)
   {  return y < x;  }

   friend bool operator<=(const BstreeImpl &x, const BstreeImpl &y)
   {  return !(x > y);  }

   friend bool operator>=(const BstreeImpl &x, const BstreeImpl &y)
   {  return !(x < y);  }

   friend void swap(BstreeImpl &x, BstreeImpl &y)
   {  x.swap(y);  }

   /// @cond
   private:
   template<class Disposer>
   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n, Disposer disposer)
   {
      for(n = 0; b != e; ++n)
        this->erase_and_dispose(b++, disposer);
      return b.unconst();
   }

   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n)
   {
      for(n = 0; b != e; ++n)
        this->erase(b++);
      return b.unconst();
   }
   /// @endcond
};

//! Helper metafunction to define a \c bstree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeBstree
{
   /// @cond
   typedef typename PackOptions
      < BstreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef BstreeImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::size_type
         , PackedOptions::constant_time_size
         , BsTreeAlgorithmsValue
         , typename PackedOptions::header_holder_type
         > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};


#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6>
#else
template<class T, class ...Options>
#endif
class Bstree
   :  public MakeBstree<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeBstree
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Bstree)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Bstree()
      :  Base()
   {}

   inline explicit Bstree( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline Bstree( bool unique, Iterator b, Iterator e
         , const KeyCompare &cmp = KeyCompare()
         , const ValueTraits &v_traits = ValueTraits())
      :  Base(unique, b, e, cmp, v_traits)
   {}

   inline Bstree(BOOST_RV_REF(Bstree) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Bstree& operator=(BOOST_RV_REF(Bstree) x)
   {  return static_cast<Bstree &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Bstree &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Bstree) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Bstree &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Bstree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Bstree &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Bstree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Bstree &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Bstree &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Bstree &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Bstree &>(Base::container_from_iterator(it));   }
};

#endif
} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_BSTREE_HPP
