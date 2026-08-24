/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_ANY_HOOK_HPP
#define BOOST_INTRUSIVE_ANY_HOOK_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/any_node_and_algorithms.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/generic_hook.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/pointer_rebind.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//! Helper metafunction to define a \c \c any_base_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void>
#endif
struct MakeAnyBaseHook
{
   /// @cond
   typedef typename PackOptions
      < HookDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef GenericHook
   < AnyAlgorithmValue
   , AnyNodeTraits<typename PackedOptions::void_pointer>
   , typename PackedOptions::tag
   , PackedOptions::link_mode
   , AnyBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Derive a class from this hook in order to store objects of that class
//! in an intrusive container.
//!
//! The hook admits the following options: \c tag<>, \c void_pointer<> and
//! \c link_mode<>.
//!
//! \c tag<> defines a tag to identify the node.
//! The same tag value can be used in different classes, but if a class is
//! derived from more than one \c any_base_hook, then each \c any_base_hook needs its
//! unique tag.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c NormalLink, \c SafeLink).
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the container configured to use this hook.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3>
#endif
class AnyBaseHook
   :  public MakeAnyBaseHook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   AnyBaseHook();

   //! <b>Effects</b>: If link_mode is or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AnyBaseHook(const AnyBaseHook& );

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AnyBaseHook& operator=(const AnyBaseHook& );

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in a container an assertion is raised.
   //!
   //! <b>Throws</b>: Nothing.
   ~AnyBaseHook();

   //! <b>Precondition</b>: link_mode must be \c SafeLink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c container::iterator_to
   //!   will return a valid iterator.
   //!
   //! <b>Complexity</b>: Constant
   bool is_linked() const;
   #endif
};

//! Helper metafunction to define a \c \c any_member_hook that yields to the same
//! type when the same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1 = void, class O2 = void, class O3 = void>
#endif
struct MakeAnyMemberHook
{
   /// @cond
   typedef typename PackOptions
      < HookDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef GenericHook
   < AnyAlgorithmValue
   , AnyNodeTraits<typename PackedOptions::void_pointer>
   , member_tag
   , PackedOptions::link_mode
   , NoBaseHookId
   > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

//! Store this hook in a class to be inserted
//! in an intrusive container.
//!
//! The hook admits the following options: \c void_pointer<> and
//! \c link_mode<>.
//!
//! \c link_mode<> will specify the linking mode of the hook (\c NormalLink or \c SafeLink).
//!
//! \c void_pointer<> is the pointer type that will be used internally in the hook
//! and the container configured to use this hook.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class ...Options>
#else
template<class O1, class O2, class O3>
#endif
class AnyMemberHook
   :  public MakeAnyMemberHook
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      <O1, O2, O3>
      #else
      <Options...>
      #endif
      ::type
{
   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   public:
   //! <b>Effects</b>: If link_mode is or \c SafeLink
   //!   initializes the node to an unlinked state.
   //!
   //! <b>Throws</b>: Nothing.
   AnyMemberHook() BOOST_NOEXCEPT;

   //! <b>Effects</b>: If link_mode is or \c SafeLink
   //!   initializes the node to an unlinked state. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing a copy-constructor
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AnyMemberHook(const AnyMemberHook&) BOOST_NOEXCEPT;

   //! <b>Effects</b>: Empty function. The argument is ignored.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Rationale</b>: Providing an assignment operator
   //!   makes classes using the hook STL-compliant without forcing the
   //!   user to do some additional work. \c swap can be used to emulate
   //!   move-semantics.
   AnyMemberHook& operator=(const AnyMemberHook&) BOOST_NOEXCEPT;

   //! <b>Effects</b>: If link_mode is \c NormalLink, the destructor does
   //!   nothing (ie. no code is generated). If link_mode is \c SafeLink and the
   //!   object is stored in a container an assertion is raised.
   //!
   //! <b>Throws</b>: Nothing.
   ~AnyMemberHook();

   //! <b>Precondition</b>: link_mode must be \c SafeLink.
   //!
   //! <b>Returns</b>: true, if the node belongs to a container, false
   //!   otherwise. This function can be used to test whether \c container::iterator_to
   //!   will return a valid iterator.
   //!
   //! <b>Complexity</b>: Constant
   bool is_linked() const BOOST_NOEXCEPT;
   #endif
};

/// @cond

namespace detail{

BOOST_INTRUSIVE_INTERNAL_STATIC_BOOL_IS_TRUE(old_proto_value_traits_base_hook, hooktags::is_base_hook)

//!This option setter specifies that the container
//!must use the specified base hook
template<class BasicHook, template <class> class NodeTraits>
struct AnyToSomeHook
{
   typedef typename BasicHook::template pack<Empty>::proto_value_traits OldProtoValueTraits;

   template<class Base>
   struct Pack : public Base
   {
      struct ProtoValueTraits
      {
         //proto_value_traits::hooktags::is_base_hook is used by get_value_traits
         //to detect base hooks, so mark it in case BasicHook has it.
         struct Hooktags
         {
            static const bool IS_BASE_HOOK = old_proto_value_traits_base_hook_bool_is_true
               <OldProtoValueTraits>::value;
         };

         typedef OldProtoValueTraits BasicHookT;
         static const bool IS_ANY_HOOK = true;

         template<class VoidPtr>
         struct NodeTraitsFromVoidptr
         {  typedef NodeTraits<VoidPtr> Type;  };
      };
   };
};

}  //namespace detail{

/// @endcond

//!This option setter specifies that
//!any hook should behave as an slist hook
template<class BasicHook>
struct AnyToSlistHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnySlistNodeTraits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as an list hook
template<class BasicHook>
struct AnyToListHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnyListNodeTraits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as a set hook
template<class BasicHook>
struct AnyToSetHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnyRbtreeNodeTraits>
/// @endcond
{};

//!This option setter specifies that
//!any hook should behave as an avl_set hook
template<class BasicHook>
struct AnyToAvlSetHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnyAvltreeNodeTraits>
/// @endcond
{};

//!This option setter specifies that any
//!hook should behave as a bs_set hook
template<class BasicHook>
struct AnyToBsSetHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnyTreeNodeTraits>
/// @endcond
{};

//!This option setter specifies that any hook
//!should behave as an unordered set hook
template<class BasicHook>
struct AnyToUnorderedSetHook
/// @cond
   :  public detail::AnyToSomeHook<BasicHook, AnyUnorderedNodeTraits>
/// @endcond
{};


} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_ANY_HOOK_HPP
