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

#ifndef BOOST_INTRUSIVE_SLIST_NODE_HPP
#define BOOST_INTRUSIVE_SLIST_NODE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_rebind.hpp>

namespace boost {
namespace intrusive {

template<class VoidPointer>
struct SlistNode
{
   typedef typename PointerRebind<VoidPointer, SlistNode>::type   NodePtr;
   NodePtr m_next;
};

// slist_node_traits can be used with circular_slist_algorithms and supplies
// a slist_node holding the pointers needed for a singly-linked list
// it is used by slist_base_hook and slist_member_hook
template<class VoidPointer>
struct SlistNodeTraits
{
   typedef SlistNode<VoidPointer>  Node;
   typedef typename Node::NodePtr  NodePtr;
   typedef typename PointerRebind<VoidPointer, const Node>::type    ConstNodePtr;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_next(ConstNodePtr n)
   {  return n->next_;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_next(NodePtr n)
   {  return n->next_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_next(NodePtr n, NodePtr next)
   {  n->next_ = next;  }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SLIST_NODE_HPP
