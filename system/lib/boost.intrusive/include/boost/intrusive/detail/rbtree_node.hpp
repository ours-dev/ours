/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2013.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_RBTREE_NODE_HPP
#define BOOST_INTRUSIVE_RBTREE_NODE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_rebind.hpp>
#include <boost/intrusive/rbtree_algorithms.hpp>
#include <boost/intrusive/pointer_plus_bits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/tree_node.hpp>

namespace boost {
namespace intrusive {

/////////////////////////////////////////////////////////////////////////////
//                                                                         //
//                Generic node_traits for any pointer type                 //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

//This is the compact representation: 3 pointers
template<class VoidPointer>
struct CompactRbtreeNode
{
   typedef CompactRbtreeNode<VoidPointer> Node;
   typedef typename PointerRebind<VoidPointer, Node >::type         NodePtr;
   typedef typename PointerRebind<VoidPointer, const Node >::type   ConstNodePtr;
   enum Color { RedT, BlackT };
   NodePtr m_parent, m_left, m_right;
};

//This is the normal representation: 3 pointers + enum
template<class VoidPointer>
struct RbtreeNode
{
   typedef RbtreeNode<VoidPointer> Node;
   typedef typename PointerRebind<VoidPointer, Node >::type         NodePtr;
   typedef typename PointerRebind<VoidPointer, const Node >::type   ConstNodePtr;

   enum Color { RedT, BlackT };
   NodePtr m_parent, m_left, m_right;
   Color m_color;
};

//This is the default node traits implementation
//using a node with 3 generic pointers plus an enum
template<class VoidPointer>
struct DefaultRbtreeNodeTraitsImpl
{
   typedef RbtreeNode<VoidPointer> Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   typedef typename Node::Color Color;

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

   BOOST_INTRUSIVE_FORCEINLINE static Color get_color(ConstNodePtr n)
   {  return n->color_;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color get_color(NodePtr n)
   {  return n->color_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_color(NodePtr n, Color c)
   {  n->color_ = c;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color black()
   {  return Node::black_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color red()
   {  return Node::red_t;  }
};

//This is the compact node traits implementation
//using a node with 3 generic pointers
template<class VoidPointer>
struct CompactRbtreeNodeTraitsImpl
{
   typedef CompactRbtreeNode<VoidPointer> Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   typedef pointer_plus_bits<NodePtr, 1> PtrBit;

   typedef typename Node::Color Color;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return PtrBit::get_pointer(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(NodePtr n)
   {  return PtrBit::get_pointer(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  PtrBit::set_pointer(n->parent_, p);  }

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

   BOOST_INTRUSIVE_FORCEINLINE static Color get_color(ConstNodePtr n)
   {  return (Color)PtrBit::get_bits(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static Color get_color(NodePtr n)
   {  return (Color)PtrBit::get_bits(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_color(NodePtr n, Color c)
   {  PtrBit::set_bits(n->parent_, c != 0);  }

   BOOST_INTRUSIVE_FORCEINLINE static Color black()
   {  return Node::black_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color red()
   {  return Node::red_t;  }
};

//Dispatches the implementation based on the boolean
template<class VoidPointer, bool Compact>
struct RbtreeNodeTraitsDispatch
   :  public DefaultRbtreeNodeTraitsImpl<VoidPointer>
{};

template<class VoidPointer>
struct RbtreeNodeTraitsDispatch<VoidPointer, true>
   :  public CompactRbtreeNodeTraitsImpl<VoidPointer>
{};

//Inherit from rbtree_node_traits_dispatch depending on the embedding capabilities
template<class VoidPointer, bool OptimizeSize = false>
struct RbtreeNodeTraits
   :  public RbtreeNodeTraitsDispatch
         < VoidPointer
         ,  OptimizeSize &&
           (MaxPointerPlusBits
            < VoidPointer
            , detail::alignment_of<CompactRbtreeNode<VoidPointer> >::value
            >::value >= 1)
         >
{};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_RBTREE_NODE_HPP
