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

#ifndef BOOST_INTRUSIVE_DETAIL_DEFAULT_HEADER_HOLDER_HPP
#define BOOST_INTRUSIVE_DETAIL_DEFAULT_HEADER_HOLDER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>

namespace boost {
namespace intrusive {
namespace detail {

// trivial header node holder
template < typename TNodeTraits >
struct DefaultHeaderHolder : public TNodeTraits::node
{
   typedef TNodeTraits NodeTraits;
   typedef typename NodeTraits::Node Node;
   typedef typename NodeTraits::NodePtr NodePtr;
   typedef typename NodeTraits::ConstNodePtr ConstNodePtr;

   DefaultHeaderHolder() : Node() {}

   inline ConstNodePtr get_node() const
   { return PointerTraits< ConstNodePtr >::pointer_to(*static_cast< const Node* >(this)); }

   inline NodePtr get_node()
   { return PointerTraits< NodePtr >::pointer_to(*static_cast< Node* >(this)); }

   // (unsafe) downcast used to implement container-from-iterator
   inline static DefaultHeaderHolder* get_holder(NodePtr p)
   { return static_cast< DefaultHeaderHolder* >(boost::movelib::to_raw_pointer(p)); }
};

// type function producing the header node holder
template < typename ValueTraits, typename HeaderHolder >
struct GetHeaderHolderType
{
   typedef HeaderHolder Type;
};
template < typename ValueTraits >
struct GetHeaderHolderType< ValueTraits, void >
{
   typedef DefaultHeaderHolder< typename ValueTraits::node_traits > Type;
};

} //namespace detail
} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DETAIL_DEFAULT_HEADER_HOLDER_HPP
