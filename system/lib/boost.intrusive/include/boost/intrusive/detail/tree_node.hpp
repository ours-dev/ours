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

#ifndef BOOST_INTRUSIVE_TREE_NODE_HPP
#define BOOST_INTRUSIVE_TREE_NODE_HPP

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
struct TreeNode
{
   typedef typename PointerRebind<VoidPointer, TreeNode>::type  NodePtr;

   NodePtr m_parent, m_left, m_right;
};

template<class VoidPointer>
struct TreeNodeTraits
{
   typedef TreeNode<VoidPointer> Node;

   typedef typename Node::NodePtr   NodePtr;
   typedef typename PointerRebind<VoidPointer, const Node>::type ConstNodePtr;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return n->parent_;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(NodePtr n)
   {  return n->parent_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  n->parent_ = p;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(ConstNodePtr n)
   {  return n->left_;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(NodePtr n)
   {  return n->left_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_left(NodePtr n, NodePtr l)
   {  n->left_ = l;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(ConstNodePtr n)
   {  return n->right_;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(NodePtr n)
   {  return n->right_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_right(NodePtr n, NodePtr r)
   {  n->right_ = r;  }
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREE_NODE_HPP
