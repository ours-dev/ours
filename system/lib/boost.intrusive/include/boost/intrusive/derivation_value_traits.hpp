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

#ifndef BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
#define BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/pointer_traits.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//!This value traits template is used to create value traits
//!from user defined node traits where value_traits::value_type will
//!derive from node_traits::node

template<class T, class TNodeTraits, LinkModeType LinkMode
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   = SafeLink
   #endif
>
struct DerivationValueTraits
{
   public:
   typedef TNodeTraits                                                NodeTraits;
   typedef T                                                         ValueType;
   typedef typename NodeTraits::Node                                Node;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<ValueType>::type                      Pointer;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const ValueType>::type                ConstPointer;
   typedef typename boost::intrusive::
      PointerTraits<Pointer>::Reference                             Reference;
   typedef typename boost::intrusive::
      PointerTraits<ConstPointer>::reference                       ConstReference;
   static const LinkModeType LINK_MODE = LinkMode;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr to_node_ptr(Reference value) BOOST_NOEXCEPT
   { return NodePtr(&value); }

   BOOST_INTRUSIVE_FORCEINLINE static ConstNodePtr to_node_ptr(ConstReference value) BOOST_NOEXCEPT
   { return ConstNodePtr(&value); }

   BOOST_INTRUSIVE_FORCEINLINE static Pointer to_value_ptr(NodePtr n) BOOST_NOEXCEPT
   {
      return PointerTraits<Pointer>::pointer_to(static_cast<Reference>(*n));
   }

   BOOST_INTRUSIVE_FORCEINLINE static ConstPointer to_value_ptr(ConstNodePtr n) BOOST_NOEXCEPT
   {
      return PointerTraits<ConstPointer>::pointer_to(static_cast<ConstReference>(*n));
   }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_DERIVATION_VALUE_TRAITS_HPP
