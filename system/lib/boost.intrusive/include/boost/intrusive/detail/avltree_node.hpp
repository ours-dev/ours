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

#ifndef BOOST_INTRUSIVE_AVLTREE_NODE_HPP
#define BOOST_INTRUSIVE_AVLTREE_NODE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_rebind.hpp>
#include <boost/intrusive/avltree_algorithms.hpp>
#include <boost/intrusive/pointer_plus_bits.hpp>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {

/////////////////////////////////////////////////////////////////////////////
//                                                                         //
//                Generic node_traits for any pointer type                 //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

//This is the compact representation: 3 pointers
template<class VoidPointer>
struct CompactAvltreeNode
{
   typedef typename PointerRebind<VoidPointer, CompactAvltreeNode<VoidPointer> >::type       NodePtr;
   typedef typename PointerRebind<VoidPointer, const CompactAvltreeNode<VoidPointer> >::type ConstNodePtr;
   enum Balance { NegativeT, ZeroT, PositiveT };
   NodePtr m_parent, m_left, m_right;
};

//This is the normal representation: 3 pointers + enum
template<class VoidPointer>
struct AvltreeNode
{
   typedef typename PointerRebind<VoidPointer, AvltreeNode<VoidPointer> >::type         NodePtr;
   typedef typename PointerRebind<VoidPointer, const AvltreeNode<VoidPointer> >::type   ConstNodePtr;
   enum Balance { NegativeT, ZeroT, PositiveT };
   NodePtr m_parent, m_left, m_right;
   Balance m_balance;
};

//This is the default node traits implementation
//using a node with 3 generic pointers plus an enum
template<class VoidPointer>
struct DefaultAvltreeNodeTraitsImpl
{
   typedef AvltreeNode<VoidPointer>      Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   typedef typename Node::Balance Balance;

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

   BOOST_INTRUSIVE_FORCEINLINE static Balance get_balance(ConstNodePtr n)
   {  return n->balance_;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance get_balance(NodePtr n)
   {  return n->balance_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_balance(NodePtr n, Balance b)
   {  n->balance_ = b;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance negative()
   {  return Node::negative_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance zero()
   {  return Node::zero_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance positive()
   {  return Node::positive_t;  }
};

//This is the compact node traits implementation
//using a node with 3 generic pointers
template<class VoidPointer>
struct CompactAvltreeNodeTraitsImpl
{
   typedef CompactAvltreeNode<VoidPointer> Node;
   typedef typename Node::NodePtr           NodePtr;
   typedef typename Node::ConstNodePtr     ConstNodePtr;
   typedef typename Node::Balance Balance;

   typedef pointer_plus_bits<NodePtr, 2> PtrBit;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return PtrBit::get_pointer(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  PtrBit::set_pointer(n->parent_, p);  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(ConstNodePtr n)
   {  return n->left_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_left(NodePtr n, NodePtr l)
   {  n->left_ = l;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(ConstNodePtr n)
   {  return n->right_;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_right(NodePtr n, NodePtr r)
   {  n->right_ = r;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance get_balance(ConstNodePtr n)
   {  return (Balance)PtrBit::get_bits(n->parent_);  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_balance(NodePtr n, Balance b)
   {  PtrBit::set_bits(n->parent_, (std::size_t)b);  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance negative()
   {  return Node::negative_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance zero()
   {  return Node::zero_t;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance positive()
   {  return Node::positive_t;  }
};

//Dispatches the implementation based on the boolean
template<class VoidPointer, bool Compact>
struct AvltreeNodeTraitsDispatch
   :  public DefaultAvltreeNodeTraitsImpl<VoidPointer>
{};

template<class VoidPointer>
struct AvltreeNodeTraitsDispatch<VoidPointer, true>
   :  public CompactAvltreeNodeTraitsImpl<VoidPointer>
{};

//Inherit from rbtree_node_traits_dispatch depending on the embedding capabilities
template<class VoidPointer, bool OptimizeSize = false>
struct AvltreeNodeTraits
   :  public AvltreeNodeTraitsDispatch
         < VoidPointer
         , OptimizeSize &&
            MaxPointerPlusBits
            < VoidPointer
            , detail::alignment_of<CompactAvltreeNode<VoidPointer> >::value
            >::value >= 2u
         >
{};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_AVLTREE_NODE_HPP
