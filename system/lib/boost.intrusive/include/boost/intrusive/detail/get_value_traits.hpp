/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_GET_VALUE_TRAITS_HPP
#define BOOST_INTRUSIVE_DETAIL_GET_VALUE_TRAITS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/hook_traits.hpp>

namespace boost {
namespace intrusive {

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

template<class SupposedValueTraits>
struct IsDefaultHookTag
{  static const bool VALUE = false;  };

namespace detail{

template <class T, class BaseHook>
struct ConcreteHookBaseValueTraits
{
   typedef typename BaseHook::hooktags Tags;
   typedef Bhtraits
      < T
      , typename Tags::node_traits
      , Tags::link_mode
      , typename Tags::tag
      , Tags::Type> Type;
};

template <class BaseHook>
struct ConcreteHookBaseValueTraits<void, BaseHook>
{
   typedef typename BaseHook::hooktags Type;
};

template <class T, class AnyToSomeHookProtoValueTraits>
struct AnyHookBaseValueTraits
{
   //AnyToSomeHook value_traits derive from a generic_hook
   //The generic_hook is configured with any_node_traits
   //and AnyToSomeHook::value_traits with the correct
   //node traits for the container, so use node_traits
   //from AnyToSomeHook_ProtoValueTraits and the rest of
   //elements from the hooktags member of the generic_hook

   typedef typename AnyToSomeHookProtoValueTraits::BasicHookT     BasicHookT;
   typedef typename PointerRebind
      < typename BasicHookT::hooktags::node_traits::node_ptr
      , void>::type                                                  VoidPointer;
   typedef typename AnyToSomeHookProtoValueTraits::template
      node_traits_from_voidptr<VoidPointer>::type                   NodeTraits;

   typedef Bhtraits
      < T
      , NodeTraits
      , BasicHookT::hooktags::link_mode
      , typename BasicHookT::hooktags::tag
      , BasicHookT::hooktags::Type
      > Type;
};

template <class AnyToSomeHookProtoValueTraits>
struct AnyHookBaseValueTraits<void, AnyToSomeHookProtoValueTraits>
{
   typedef typename AnyToSomeHookProtoValueTraits::BasicHookT     BasicHookT;
   typedef typename PointerRebind
      < typename BasicHookT::hooktags::node_traits::node_ptr
      , void>::type                                                  VoidPointer;

   struct Type
   {
      typedef typename AnyToSomeHookProtoValueTraits::template
         node_traits_from_voidptr<VoidPointer>::Type                NodeTraits;
   };
};

template<class MemberHook>
struct GetMemberValueTraits
{
   typedef typename MemberHook::member_value_traits Type;
};

BOOST_INTRUSIVE_INTERNAL_STATIC_BOOL_IS_TRUE(internal_any_hook, is_any_hook)
BOOST_INTRUSIVE_INTERNAL_STATIC_BOOL_IS_TRUE(internal_base_hook, hooktags::is_base_hook)

template <class T>
struct InternalMemberValueTraits
{
   template <class U> static yes_type test(...);
   template <class U> static no_type test(typename U::member_value_traits* = 0);
   static const bool VALUE = sizeof(test<T>(0)) == sizeof(no_type);
};

template<class SupposedValueTraits, class T, bool = IsDefaultHookTag<SupposedValueTraits>::value>
struct supposed_value_traits;

template<class T, class BaseHook, bool = internal_any_hook_bool_is_true<BaseHook>::value>
struct get_base_value_traits;

template<class SupposedValueTraits, class T, bool = internal_base_hook_bool_is_true<SupposedValueTraits>::value>
struct supposed_base_value_traits;

template<class SupposedValueTraits, bool = InternalMemberValueTraits<SupposedValueTraits>::value>
struct supposed_member_value_traits;

template<class SupposedValueTraits, bool = internal_any_hook_bool_is_true<SupposedValueTraits>::value>
struct any_or_concrete_value_traits;

//Base any hook
template<class T, class BaseHook>
struct get_base_value_traits<T, BaseHook, true>
   : AnyHookBaseValueTraits<T, BaseHook>
{};

//Non-any base hook
template<class T, class BaseHook>
struct get_base_value_traits<T, BaseHook, false>
   : ConcreteHookBaseValueTraits<T, BaseHook>
{};

//...It's a default hook
template<class SupposedValueTraits, class T>
struct supposed_value_traits<SupposedValueTraits, T, true>
{  typedef typename SupposedValueTraits::template apply<T>::Type Type;  };

//...Not  a default hook
template<class SupposedValueTraits, class T>
struct supposed_value_traits<SupposedValueTraits, T, false>
{  typedef SupposedValueTraits Type;   };

//...It's a base hook
template<class BaseHook, class T>
struct supposed_base_value_traits<BaseHook, T, true>
   : get_base_value_traits<T, BaseHook>
{};

//...Not a base hook, try if it's a member or value_traits
template<class SupposedValueTraits, class T>
struct supposed_base_value_traits<SupposedValueTraits, T, false>
   : supposed_member_value_traits<SupposedValueTraits>
{};

//...It's a member hook
template<class MemberHook>
struct supposed_member_value_traits<MemberHook, true>
   : GetMemberValueTraits<MemberHook>
{};

//...Not a member hook
template<class SupposedValueTraits>
struct supposed_member_value_traits<SupposedValueTraits, false>
   : any_or_concrete_value_traits<SupposedValueTraits>
{};

template<class AnyToSomeHookProtoValueTraits>
struct any_or_concrete_value_traits<AnyToSomeHookProtoValueTraits, true>
{
   //A hook node (non-base, e.g.: member or other value traits
   typedef typename AnyToSomeHookProtoValueTraits::BasicHookT        BasicHookT;
   typedef typename PointerRebind
      <typename BasicHookT::node_ptr, void>::type                     VoidPointer;
   typedef typename AnyToSomeHookProtoValueTraits::template
      node_traits_from_voidptr<VoidPointer>::type                      AnyNodeTraits;

   struct Type : BasicHookT
   {
      typedef AnyNodeTraits NodeTraits;
   };
};

template<class SupposedValueTraits>
struct any_or_concrete_value_traits<SupposedValueTraits, false>
{
   typedef SupposedValueTraits Type;
};

////////////////////////////////////////
//  get_value_traits / get_node_traits
////////////////////////////////////////

template<class T, class SupposedValueTraits>
struct GetValueTraits
   : supposed_base_value_traits<typename supposed_value_traits<SupposedValueTraits, T>::type, T>
{};

template<class SupposedValueTraits>
struct GetNodeTraits
{
   typedef typename GetValueTraits<void, SupposedValueTraits>::Type::node_traits Type;
};

}  //namespace detail{

#endif   //BOOST_INTRUSIVE_DOXYGEN_INVOKED

}  //namespace intrusive {
}  //namespace boost {

#include <boost/intrusive/detail/config_end.hpp>

#endif   //#ifndef BOOST_INTRUSIVE_DETAIL_GET_VALUE_TRAITS_HPP
