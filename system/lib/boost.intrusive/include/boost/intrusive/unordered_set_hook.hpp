/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_UNORDERED_SET_HOOK_HPP
#define BOOST_INTRUSIVE_UNORDERED_SET_HOOK_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/slist_hook.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/generic_hook.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

template<class VoidPointer, bool StoreHash, bool OptimizeMultiKey>
struct UnorderedNode
   :  public SlistNode<VoidPointer>
{
   typedef typename PointerTraits
      <VoidPointer>::template rebind_pointer
         < UnorderedNode<VoidPointer, StoreHash, OptimizeMultiKey> >::type
      NodePtr;
   NodePtr    m_prev_in_group;
   std::size_t m_hash;
};

template<class VoidPointer>
struct UnorderedNode<VoidPointer, false, true>
   :  public SlistNode<VoidPointer>
{
   typedef typename PointerTraits
      <VoidPointer>::template rebind_pointer
         < UnorderedNode<VoidPointer, false, true> >::type
      NodePtr;
   NodePtr    m_prev_in_group;
};

template<class VoidPointer>
struct UnorderedNode<VoidPointer, true, false>
   :  public SlistNode<VoidPointer>
{
   typedef typename PointerTraits
      <VoidPointer>::template rebind_pointer
         < UnorderedNode<VoidPointer, true, false> >::type
      NodePtr;
   std::size_t m_hash;
};

template<class VoidPointer, bool StoreHash, bool OptimizeMultiKey>
struct UnorderedNodeTraits
   :  public SlistNodeTraits<VoidPointer>
{
   typedef SlistNodeTraits<VoidPointer> ReducedSlistNodeTraits;
   typedef UnorderedNode<VoidPointer, StoreHash, OptimizeMultiKey> Node;

   typedef typename PointerTraits
      <VoidPointer>::template rebind_pointer
         < Node >::type NodePtr;
   typedef typename PointerTraits
      <VoidPointer>::template rebind_pointer
         < const Node >::type ConstNodePtr;

   static const bool STORE_HASH        = StoreHash;
   static const bool OPTIMIZE_MULTIKEY = OptimizeMultiKey;

   inline static NodePtr get_next(ConstNodePtr n) BOOST_NOEXCEPT
   {  return PointerTraits<NodePtr>::static_cast_from(n->next_);  }

   inline static void set_next(NodePtr n, NodePtr next) BOOST_NOEXCEPT
   {  n->next_ = next;  }

   inline static NodePtr get_prev_in_group(ConstNodePtr n) BOOST_NOEXCEPT
   {  return n->prev_in_group_;  }

   inline static void set_prev_in_group(NodePtr n, NodePtr prev) BOOST_NOEXCEPT
   {  n->prev_in_group_ = prev;  }

   inline static std::size_t get_hash(ConstNodePtr n) BOOST_NOEXCEPT
   {  return n->hash_;  }

   inline static void set_hash(NodePtr n, std::size_t h) BOOST_NOEXCEPT
   {  n->hash_ = h;  }
};

template<class NodeTraits>
struct UnorderedGroupAdapter
{
   typedef typename NodeTraits::Node            Node;
   typedef typename NodeTraits::NodePtr        NodePtr;
   typedef typename NodeTraits::ConstNodePtr  ConstNodePtr;

   inline static NodePtr get_next(ConstNodePtr n)
   {  return NodeTraits::get_prev_in_group(n);  }

   inline static void set_next(NodePtr n, NodePtr next)
   {  NodeTraits::set_prev_in_group(n, next);   }
};

template<class TNodeTraits>
struct UnorderedAlgorithms
   : public CircularSlistAlgorithms<TNodeTraits>
{
   typedef CircularSlistAlgorithms<TNodeTraits>   BaseType;
   typedef UnorderedGroupAdapter<TNodeTraits>     GroupTraits;
   typedef CircularSlistAlgorithms<GroupTraits> GroupAlgorithms;
   typedef TNodeTraits                              NodeTraits;
   typedef typename TNodeTraits::Node               Node;
   typedef typename TNodeTraits::NodePtr           NodePtr;
   typedef typename TNodeTraits::ConstNodePtr     ConstNodePtr;

   inline static void init(typename BaseType::node_ptr n) BOOST_NOEXCEPT
   {
      BaseType::init(n);
      GroupAlgorithms::init(n);
   }

   inline static void init_header(typename BaseType::node_ptr n) BOOST_NOEXCEPT
   {
      BaseType::init_header(n);
      GroupAlgorithms::init_header(n);
   }

   inline static void unlink(typename BaseType::node_ptr n) BOOST_NOEXCEPT
   {
      BaseType::unlink(n);
      GroupAlgorithms::unlink(n);
   }
};

//Class to avoid defining the same algo as a circular list, as hooks would be ambiguous between them
template<class Algo>
struct UsetAlgoWrapper : public Algo
{};

template<class VoidPointer, bool StoreHash, bool OptimizeMultiKey>
struct GetUsetNodeTraits
{
   typedef typename detail::if_c
      < (StoreHash || OptimizeMultiKey)
      , UnorderedNodeTraits<VoidPointer, StoreHash, OptimizeMultiKey>
      , SlistNodeTraits<VoidPointer>
      >::Type Type;
};

template<bool OptimizeMultiKey>
struct GetUsetAlgoType
{
   static const AlgoTypes VALUE = OptimizeMultiKey ? UnorderedAlgorithmsValue : UnorderedCircularSlistAlgorithmsValue;
};

template<class NodeTraits>
struct get_algo<UnorderedAlgorithmsValue, NodeTraits>
{
   typedef UnorderedAlgorithms<NodeTraits> Type;
};

template<class NodeTraits>
struct get_algo<UnorderedCircularSlistAlgorithmsValue, NodeTraits>
{
   typedef UsetAlgoWrapper< CircularSlistAlgorithms<NodeTraits> > Type;
};

/// @endcond

//! Helper metafunction to define a \c unordered_set_base_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void, class O4 = void>
#endif
struct MakeUnorderedSetBaseHook
{
   /// @cond
   typedef typename PackOptions
      < HookDefaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type PackedOptions;

   typedef GenericHook
   < GetUsetAlgoType <PackedOptions::optimize_multikey>::value
   , typename GetUsetNodeTraits < typename PackedOptions::void_pointer
                                   , PackedOptions::store_hash
                                   , PackedOptions::optimize_multikey
                                   >::type
   , typename PackedOptions::tag
   , PackedOptions::link_mode
   , HashBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Derive a class from unordered_set_base_hook in order to store objects in
//! in an unordered_set/unordered_multi_set. unordered_set_base_hook holds the data necessary to maintain
//! the unordered_set/unordered_multi_set and provides an appropriate value_traits class for unordered_set/unordered_multi_set.
//!
//! The hook admits the following options: \c tag<>, \c void_pointer<>,
//! \c link_mode<>, \c store_hash<> and \c optimize_multikey<>.
//!
//! \c tag<> defines a tag to identify the node.
//! The same tag value can be used in different classes, but if a class is
//! derived from more than one \c list_base_hook, then each \c list_base_hook needs its
//! unique tag.
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the container configured to use this hook.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c NormalLink,
//! \c AutoUnlink or \c SafeLink).
//!
//! \c store_hash<> will tell the hook to store the hash of the value
//! to speed up rehashings.
//!
//! \c optimize_multikey<> will tell the hook to store a link to form a group
//! with other value with the same value to speed up searches and insertions
//! in unordered_multisets with a great number of with equivalent keys.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3, class O4>
#endif
class UnorderedSetBaseHook
   :  public MakeUnorderedSetBaseHook<
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   UnorderedSetBaseHook() BOOST_NOEXCEPT;

   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   UnorderedSetBaseHook(const UnorderedSetBaseHook& ) BOOST_NOEXCEPT;

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   UnorderedSetBaseHook& operator=(const UnorderedSetBaseHook& ) BOOST_NOEXCEPT;

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in an unordered_set an assertion is raised. If link_mode is
   //!   \c AutoUnlink and \c is_linked() is true, the node is unlinked.
   //!
   //! <b>Throws</b>: Nothing.
   ~UnorderedSetBaseHook();

   //! <b>Effects</b>: Swapping two nodes swaps the position of the elements
   //!   related to those nodes in one or two containers. That is, if the node
   //!   this is part of the element e1, the node x is part of the element e2
   //!   and both elements are included in the containers s1 and s2, then after
   //!   the swap-operation e1 is in s2 at the position of e2 and e2 is in s1
   //!   at the position of e1. If one element is not in a container, then
   //!   after the swap-operation the other element is not in a container.
   //!   Iterators to e1 and e2 related to those nodes are invalidated.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   void swap_nodes(UnorderedSetBaseHook &other) BOOST_NOEXCEPT;

   //! <b>Precondition</b>: link_mode must be \c SafeLink or \c AutoUnlink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c unordered_set::iterator_to
   //!   will return a valid iterator.
   //!
   //! <b>Complexity</b>: Constant
   bool is_linked() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Removes the node if it's inserted in a container.
   //!   This function is only allowed if link_mode is \c AutoUnlink.
   //!
   //! <b>Throws</b>: Nothing.
   void unlink() BOOST_NOEXCEPT;
   #endif
};


//! Helper metafunction to define a \c unordered_set_member_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void, class O4 = void>
#endif
struct MakeUnorderedSetMemberHook
{
   /// @cond
   typedef typename PackOptions
      < HookDefaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type PackedOptions;

   typedef GenericHook
   < GetUsetAlgoType <PackedOptions::optimize_multikey>::value
   , typename GetUsetNodeTraits < typename PackedOptions::void_pointer
                                   , PackedOptions::store_hash
                                   , PackedOptions::optimize_multikey
                                   >::type
   , member_tag
   , PackedOptions::link_mode
   , NoBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Put a public data member unordered_set_member_hook in order to store objects of this class in
//! an unordered_set/unordered_multi_set. unordered_set_member_hook holds the data necessary for maintaining the
//! unordered_set/unordered_multi_set and provides an appropriate value_traits class for unordered_set/unordered_multi_set.
//!
//! The hook admits the following options: \c void_pointer<>,
//! \c link_mode<> and \c store_hash<>.
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the container configured to use this hook.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c NormalLink,
//! \c AutoUnlink or \c SafeLink).
//!
//! \c store_hash<> will tell the hook to store the hash of the value
//! to speed up rehashings.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3, class O4>
#endif
class UnorderedSetMemberHook
   :  public MakeUnorderedSetMemberHook<
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
   >::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   UnorderedSetMemberHook();

   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   UnorderedSetMemberHook(const UnorderedSetMemberHook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   UnorderedSetMemberHook& operator=(const UnorderedSetMemberHook& );

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in an unordered_set an assertion is raised. If link_mode is
   //!   \c AutoUnlink and \c is_linked() is true, the node is unlinked.
   //!
   //! <b>Throws</b>: Nothing.
   ~UnorderedSetMemberHook();

   //! <b>Effects</b>: Swapping two nodes swaps the position of the elements
   //!   related to those nodes in one or two containers. That is, if the node
   //!   this is part of the element e1, the node x is part of the element e2
   //!   and both elements are included in the containers s1 and s2, then after
   //!   the swap-operation e1 is in s2 at the position of e2 and e2 is in s1
   //!   at the position of e1. If one element is not in a container, then
   //!   after the swap-operation the other element is not in a container.
   //!   Iterators to e1 and e2 related to those nodes are invalidated.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   void swap_nodes(UnorderedSetMemberHook &other);

   //! <b>Precondition</b>: link_mode must be \c SafeLink or \c AutoUnlink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c unordered_set::iterator_to
   //!   will return a valid iterator.
   //!
   //! <b>Complexity</b>: Constant
   bool is_linked() const;

   //! <b>Effects</b>: Removes the node if it's inserted in a container.
   //!   This function is only allowed if link_mode is \c AutoUnlink.
   //!
   //! <b>Throws</b>: Nothing.
   void unlink();
   #endif
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_UNORDERED_SET_HOOK_HPP
