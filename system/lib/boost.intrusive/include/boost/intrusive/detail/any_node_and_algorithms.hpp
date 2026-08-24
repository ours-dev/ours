/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_ANY_NODE_HPP
#define BOOST_INTRUSIVE_ANY_NODE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_rebind.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <cstddef>

namespace boost {
namespace intrusive {

template<class VoidPointer>
struct AnyNode
{
   typedef AnyNode                                               Node;
   typedef typename PointerRebind<VoidPointer, Node>::type       NodePtr;
   typedef typename PointerRebind<VoidPointer, const Node>::type ConstNodePtr;
   NodePtr    m_node_ptr_1;
   NodePtr    m_node_ptr_2;
   NodePtr    m_node_ptr_3;
   std::size_t m_size_t_1;
};

template<class VoidPointer>
struct AnyListNodeTraits
{
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_next(ConstNodePtr n)
   {  return n->node_ptr_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_next(NodePtr n, NodePtr next)
   {  n->node_ptr_1 = next;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_previous(ConstNodePtr n)
   {  return n->node_ptr_2;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_previous(NodePtr n, NodePtr prev)
   {  n->node_ptr_2 = prev;  }
};


template<class VoidPointer>
struct AnySlistNodeTraits
{
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_next(ConstNodePtr n)
   {  return n->node_ptr_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_next(NodePtr n, NodePtr next)
   {  n->node_ptr_1 = next;  }
};


template<class VoidPointer>
struct AnyUnorderedNodeTraits
   :  public AnySlistNodeTraits<VoidPointer>
{
   typedef AnySlistNodeTraits<VoidPointer>                  ReducedSlistNodeTraits;
   typedef typename ReducedSlistNodeTraits::Node            Node;
   typedef typename ReducedSlistNodeTraits::NodePtr        NodePtr;
   typedef typename ReducedSlistNodeTraits::ConstNodePtr  ConstNodePtr;

   static const bool STORE_HASH        = true;
   static const bool OPTIMIZE_MULTIKEY = true;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_next(ConstNodePtr n)
   {  return n->node_ptr_1;   }

   BOOST_INTRUSIVE_FORCEINLINE static void set_next(NodePtr n, NodePtr next)
   {  n->node_ptr_1 = next;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_prev_in_group(ConstNodePtr n)
   {  return n->node_ptr_2;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_prev_in_group(NodePtr n, NodePtr prev)
   {  n->node_ptr_2 = prev;  }

   BOOST_INTRUSIVE_FORCEINLINE static std::size_t get_hash(ConstNodePtr n)
   {  return n->size_t_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_hash(NodePtr n, std::size_t h)
   {  n->size_t_1 = h;  }
};


template<class VoidPointer>
struct AnyRbtreeNodeTraits
{
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   typedef std::size_t Color;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return n->node_ptr_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  n->node_ptr_1 = p;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(ConstNodePtr n)
   {  return n->node_ptr_2;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_left(NodePtr n, NodePtr l)
   {  n->node_ptr_2 = l;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(ConstNodePtr n)
   {  return n->node_ptr_3;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_right(NodePtr n, NodePtr r)
   {  n->node_ptr_3 = r;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color get_color(ConstNodePtr n)
   {  return n->size_t_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_color(NodePtr n, Color c)
   {  n->size_t_1 = c;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color black()
   {  return 0u;  }

   BOOST_INTRUSIVE_FORCEINLINE static Color red()
   {  return 1u;  }
};


template<class VoidPointer>
struct AnyAvltreeNodeTraits
{
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   typedef std::size_t Balance;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return n->node_ptr_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  n->node_ptr_1 = p;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(ConstNodePtr n)
   {  return n->node_ptr_2;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_left(NodePtr n, NodePtr l)
   {  n->node_ptr_2 = l;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(ConstNodePtr n)
   {  return n->node_ptr_3;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_right(NodePtr n, NodePtr r)
   {  n->node_ptr_3 = r;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance get_balance(ConstNodePtr n)
   {  return n->size_t_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_balance(NodePtr n, Balance b)
   {  n->size_t_1 = b;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance negative()
   {  return 0u;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance zero()
   {  return 1u;  }

   BOOST_INTRUSIVE_FORCEINLINE static Balance positive()
   {  return 2u;  }
};


template<class VoidPointer>
struct AnyTreeNodeTraits
{
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_parent(ConstNodePtr n)
   {  return n->node_ptr_1;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_parent(NodePtr n, NodePtr p)
   {  n->node_ptr_1 = p;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_left(ConstNodePtr n)
   {  return n->node_ptr_2;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_left(NodePtr n, NodePtr l)
   {  n->node_ptr_2 = l;  }

   BOOST_INTRUSIVE_FORCEINLINE static NodePtr get_right(ConstNodePtr n)
   {  return n->node_ptr_3;  }

   BOOST_INTRUSIVE_FORCEINLINE static void set_right(NodePtr n, NodePtr r)
   {  n->node_ptr_3 = r;  }
};

template<class VoidPointer>
class AnyNodeTraits
{
   public:
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;
};

template<class VoidPointer>
class AnyAlgorithms
{
   template <class T>
   static void function_not_available_for_any_hooks(typename detail::enable_if<detail::is_same<T, bool> >::type)
   {}

   public:
   typedef AnyNode<VoidPointer>          Node;
   typedef typename Node::NodePtr        NodePtr;
   typedef typename Node::ConstNodePtr  ConstNodePtr;
   typedef AnyNodeTraits<VoidPointer>   NodeTraits;

   //! <b>Requires</b>: 'n' must not be part of any tree.
   //!
   //! <b>Effects</b>: After the function unique(node) == true.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If node is inserted in a tree, this function corrupts the tree.
   inline static void init(NodePtr n) BOOST_NOEXCEPT
   {  n->node_ptr_1 = NodePtr();   };

   //! <b>Effects</b>: Returns true if 'n' is in the same state as if called init(node)
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool inited(ConstNodePtr n)
   {  return !n->node_ptr_1;  };

   inline static bool unique(ConstNodePtr n) BOOST_NOEXCEPT
   {  return !n->node_ptr_1; }

   static void unlink(NodePtr)
   {
      //Auto-unlink hooks and unlink() are not available for any hooks
      AnyAlgorithms<VoidPointer>::template function_not_available_for_any_hooks<NodePtr>();
   }

   static void swap_nodes(NodePtr, NodePtr)
   {
      //Any nodes have no swap_nodes capability because they don't know
      //what algorithm they must use to unlink the node from the container
      AnyAlgorithms<VoidPointer>::template function_not_available_for_any_hooks<NodePtr>();
   }
};

///@cond

template<class NodeTraits>
struct get_algo<AnyAlgorithmValue, NodeTraits>
{
   typedef typename PointerRebind<typename NodeTraits::node_ptr, void>::type VoidPointer;
   typedef AnyAlgorithms<VoidPointer> Type;
};

///@endcond

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_ANY_NODE_HPP
