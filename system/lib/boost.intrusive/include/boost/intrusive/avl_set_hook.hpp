/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_AVL_SET_HOOK_HPP
#define BOOST_INTRUSIVE_AVL_SET_HOOK_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/avltree_node.hpp>
#include <boost/intrusive/avltree_algorithms.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/generic_hook.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//! Helper metafunction to define a \c avl_set_base_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void, class O4 = void>
#endif
struct MakeAvlSetBaseHook
{
   /// @cond
   typedef typename PackOptions
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <hook_defaults, O1, O2, O3, O4>
      #else
      <HookDefaults, Options...>
      #endif
      ::type PackedOptions;

   typedef GenericHook
   < AvlTreeAlgorithmsValue
   , AvltreeNodeTraits<typename PackedOptions::void_pointer, PackedOptions::optimize_size>
   , typename PackedOptions::tag
   , PackedOptions::link_mode
   , AvlTreeBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Derive a class from avl_set_base_hook in order to store objects in
//! in an avl_set/avl_multiset. avl_set_base_hook holds the data necessary to maintain
//! the avl_set/avl_multiset and provides an appropriate value_traits class for avl_set/avl_multiset.
//!
//! The hook admits the following options: \c tag<>, \c void_pointer<>,
//! \c link_mode<> and \c optimize_size<>.
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
//! \c optimize_size<> will tell the hook to optimize the hook for size instead
//! of speed.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3, class O4>
#endif
class AvlSetBaseHook
   :  public MakeAvlSetBaseHook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3, O4>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   AvlSetBaseHook();

   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AvlSetBaseHook(const AvlSetBaseHook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AvlSetBaseHook& operator=(const AvlSetBaseHook& );

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in a set an assertion is raised. If link_mode is
   //!   \c AutoUnlink and \c is_linked() is true, the node is unlinked.
   //!
   //! <b>Throws</b>: Nothing.
   ~AvlSetBaseHook();

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
   void swap_nodes(AvlSetBaseHook &other);

   //! <b>Precondition</b>: link_mode must be \c SafeLink or \c AutoUnlink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c set::iterator_to
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

//! Helper metafunction to define a \c avl_set_member_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void, class O4 = void>
#endif
struct MakeAvlSetMemberHook
{
   /// @cond
   typedef typename PackOptions
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <hook_defaults, O1, O2, O3, O4>
      #else
      <HookDefaults, Options...>
      #endif
      ::type PackedOptions;

   typedef GenericHook
   < AvlTreeAlgorithmsValue
   , AvltreeNodeTraits<typename PackedOptions::void_pointer, PackedOptions::optimize_size>
   , member_tag
   , PackedOptions::link_mode
   , NoBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Put a public data member avl_set_member_hook in order to store objects of this class in
//! an avl_set/avl_multiset. avl_set_member_hook holds the data necessary for maintaining the
//! avl_set/avl_multiset and provides an appropriate value_traits class for avl_set/avl_multiset.
//!
//! The hook admits the following options: \c void_pointer<>,
//! \c link_mode<> and \c optimize_size<>.
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the container configured to use this hook.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c NormalLink,
//! \c AutoUnlink or \c SafeLink).
//!
//! \c optimize_size<> will tell the hook to optimize the hook for size instead
//! of speed.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3, class O4>
#endif
class AvlSetMemberHook
   :  public MakeAvlSetMemberHook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3, O4>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   AvlSetMemberHook();

   //! <b>Effects</b>: If link_mode is \c AutoUnlink or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AvlSetMemberHook(const AvlSetMemberHook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AvlSetMemberHook& operator=(const AvlSetMemberHook& );

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in a set an assertion is raised. If link_mode is
   //!   \c AutoUnlink and \c is_linked() is true, the node is unlinked.
   //!
   //! <b>Throws</b>: Nothing.
   ~AvlSetMemberHook();

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
   void swap_nodes(AvlSetMemberHook &other);

   //! <b>Precondition</b>: link_mode must be \c SafeLink or \c AutoUnlink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c set::iterator_to
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

#endif //BOOST_INTRUSIVE_AVL_SET_HOOK_HPP
