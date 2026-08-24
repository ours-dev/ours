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

#ifndef BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
#define BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/parent_from_member.hpp>
#include <boost/move/detail/to_raw_pointer.hpp> 
#include <boost/intrusive/pointer_traits.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//!This value traits template is used to create value traits
//!from user defined node traits where value_traits::value_type will
//!store a node_traits::node
template< class T, class TNodeTraits
        , typename TNodeTraits::node T::* PtrToMember
        , LinkModeType LinkMode
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   = SafeLink
   #endif
>
struct MemberValueTraits
{
   public:
   typedef TNodeTraits                                                   NodeTraits;
   typedef T                                                            ValueType;
   typedef typename NodeTraits::Node                                   Node;
   typedef typename NodeTraits::NodePtr                               NodePtr;
   typedef typename NodeTraits::ConstNodePtr                         ConstNodePtr;
   typedef PointerTraits<NodePtr>                                     NodePtrTraits;
   typedef typename PointerTraits<NodePtr>::template
      rebind_pointer<T>::type                                           Pointer;
   typedef typename PointerTraits<NodePtr>::template
      rebind_pointer<const T>::type                                     ConstPointer;
   //typedef typename pointer_traits<pointer>::reference                  reference;
   //typedef typename pointer_traits<const_pointer>::reference            const_reference;
   typedef ValueType &                                                 Reference;
   typedef const ValueType &                                           ConstReference;
   static const LinkModeType LINK_MODE = LinkMode;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr to_node_ptr(Reference value) BOOST_NOEXCEPT
   {  return PointerTraits<NodePtr>::pointer_to(value.*PtrToMember);   }

   BOOST_INTRUSIVE_FORCEINLINE static ConstNodePtr to_node_ptr(ConstReference value) BOOST_NOEXCEPT
   {  return PointerTraits<ConstNodePtr>::pointer_to(value.*PtrToMember);   }

   BOOST_INTRUSIVE_FORCEINLINE static Pointer to_value_ptr(NodePtr n) BOOST_NOEXCEPT
   {
      return PointerTraits<Pointer>::pointer_to(*detail::parent_from_member<ValueType, Node>
         (boost::movelib::to_raw_pointer(n), PtrToMember));
   }

   BOOST_INTRUSIVE_FORCEINLINE static ConstPointer to_value_ptr(ConstNodePtr n) BOOST_NOEXCEPT
   {
      return PointerTraits<ConstPointer>::pointer_to(*detail::parent_from_member<ValueType, Node>
         (boost::movelib::to_raw_pointer(n), PtrToMember));

   }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_MEMBER_VALUE_TRAITS_HPP
