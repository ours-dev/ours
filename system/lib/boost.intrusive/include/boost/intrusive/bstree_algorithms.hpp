/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2021
// (C) Copyright Daniel Steck   2021
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_BSTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_BSTREE_ALGORITHMS_HPP

#include <cstddef>
#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/bstree_algorithms_base.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/uncast.hpp>
#include <boost/intrusive/detail/math.hpp>
#include <boost/intrusive/detail/algo_type.hpp>

#include <boost/intrusive/detail/minimal_pair_header.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

//! This type is the information that will be filled by insert_unique_check
template <class NodePtr>
struct InsertCommitDataT
{
   inline InsertCommitDataT()
      : m_link_left(false), m_node()
   {}
   bool     m_link_left;
   NodePtr  m_node;
};

template <class NodePtr>
struct DataForRebalanceT
{
   NodePtr  m_x;
   NodePtr  m_x_parent;
   NodePtr  m_y;
};

namespace detail {

template<class TValueTraits, class NodePtrCompare, class ExtraChecker>
struct BstreeNodeChecker
   : public ExtraChecker
{
   typedef ExtraChecker                            BaseCheckerT;
   typedef TValueTraits                             ValueTraits;
   typedef typename ValueTraits::NodeTraits      NodeTraits;
   typedef typename NodeTraits::ConstNodePtr    ConstNodePtr;

   struct ReturnType
      : public BaseCheckerT::ReturnType
   {
      inline ReturnType()
         : m_min_key_node_ptr(ConstNodePtr()), m_max_key_node_ptr(ConstNodePtr()), m_node_count(0)
      {}

      ConstNodePtr m_min_key_node_ptr;
      ConstNodePtr m_max_key_node_ptr;
      size_t   m_node_count;
   };

   inline BstreeNodeChecker(const NodePtrCompare& comp, ExtraChecker extra_checker)
      : BaseCheckerT(extra_checker), m_comp(comp)
   {}

   void operator () (ConstNodePtr p,
                     const ReturnType& check_return_left, const ReturnType& check_return_right,
                     ReturnType& check_return)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!check_return_left.m_max_key_node_ptr || !m_comp(p, check_return_left.m_max_key_node_ptr));
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!check_return_right.m_min_key_node_ptr || !m_comp(check_return_right.m_min_key_node_ptr, p));
      check_return.m_min_key_node_ptr = NodeTraits::get_left(p)? check_return_left.m_min_key_node_ptr : p;
      check_return.m_max_key_node_ptr = NodeTraits::get_right(p)? check_return_right.m_max_key_node_ptr : p;
      check_return.m_node_count = check_return_left.m_node_count + check_return_right.m_node_count + 1;
      BaseCheckerT::operator()(p, check_return_left, check_return_right, check_return);
   }

   const NodePtrCompare m_comp;
};

} // namespace detail

/// @endcond



//!   This is an implementation of a binary search tree.
//!   A node in the search tree has references to its children and its parent. This
//!   is to allow traversal of the whole tree from a given node making the
//!   implementation of iterator a pointer to a node.
//!   At the top of the tree a node is used specially. This node's parent pointer
//!   is pointing to the root of the tree. Its left pointer points to the
//!   leftmost node in the tree and the right pointer to the rightmost one.
//!   This node is used to represent the end-iterator.
//!
//!                                            +---------+
//!       header------------------------------>|         |
//!                                            |         |
//!                   +----------(left)--------|         |--------(right)---------+
//!                   |                        +---------+                        |
//!                   |                             |                             |
//!                   |                             | (parent)                    |
//!                   |                             |                             |
//!                   |                             |                             |
//!                   |                        +---------+                        |
//!    root of tree ..|......................> |         |                        |
//!                   |                        |    D    |                        |
//!                   |                        |         |                        |
//!                   |                +-------+---------+-------+                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |                |                         |                |
//!                   |          +---------+                 +---------+          |
//!                   |          |         |                 |         |          |
//!                   |          |    B    |                 |    F    |          |
//!                   |          |         |                 |         |          |
//!                   |       +--+---------+--+           +--+---------+--+       |
//!                   |       |               |           |               |       |
//!                   |       |               |           |               |       |
//!                   |       |               |           |               |       |
//!                   |   +---+-----+   +-----+---+   +---+-----+   +-----+---+   |
//!                   +-->|         |   |         |   |         |   |         |<--+
//!                       |    A    |   |    C    |   |    E    |   |    G    |
//!                       |         |   |         |   |         |   |         |
//!                       +---------+   +---------+   +---------+   +---------+
//!
//! bstree_algorithms is configured with a NodeTraits class, which encapsulates the
//! information about the node to be manipulated. NodeTraits must support the
//! following interface:
//!
//! <b>Typedefs</b>:
//!
//! <tt>node</tt>: The type of the node that forms the binary search tree
//!
//! <tt>node_ptr</tt>: A pointer to a node
//!
//! <tt>const_node_ptr</tt>: A pointer to a const node
//!
//! <b>Static functions</b>:
//!
//! <tt>static node_ptr get_parent(const_node_ptr n);</tt>
//!
//! <tt>static void set_parent(node_ptr n, node_ptr parent);</tt>
//!
//! <tt>static node_ptr get_left(const_node_ptr n);</tt>
//!
//! <tt>static void set_left(node_ptr n, node_ptr left);</tt>
//!
//! <tt>static node_ptr get_right(const_node_ptr n);</tt>
//!
//! <tt>static void set_right(node_ptr n, node_ptr right);</tt>
template<class TNodeTraits>
class BstreeAlgorithms : public BstreeAlgorithmsBase<TNodeTraits>
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;
   typedef InsertCommitDataT<NodePtr>       InsertCommitData;
   typedef DataForRebalanceT<NodePtr>       DataForRebalance;

   /// @cond
   typedef BstreeAlgorithms<TNodeTraits>        ThisType;
   typedef BstreeAlgorithmsBase<TNodeTraits>   BaseType;
   private:
   template<class Disposer>
   struct DisposeSubtreeDisposer
   {
      inline DisposeSubtreeDisposer(Disposer &disp, NodePtr subtree)
         : m_disposer(&disp), m_subtree(subtree)
      {}

      inline void release()
      {  m_disposer = 0;  }

      inline ~DisposeSubtreeDisposer()
      {
         if(m_disposer){
            dispose_subtree(m_subtree, *m_disposer);
         }
      }
      Disposer *m_disposer;
      const NodePtr m_subtree;
   };

   /// @endcond

   public:
   //! <b>Requires</b>: 'header' is the header node of a tree.
   //!
   //! <b>Effects</b>: Returns the first node of the tree, the header if the tree is empty.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   inline static NodePtr begin_node(ConstNodePtr header) BOOST_NOEXCEPT
   {  return NodeTraits::get_left(header);   }

   //! <b>Requires</b>: 'header' is the header node of a tree.
   //!
   //! <b>Effects</b>: Returns the header of the tree.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   inline static NodePtr end_node(ConstNodePtr header) BOOST_NOEXCEPT
   {  return detail::uncast(header);   }

   //! <b>Requires</b>: 'header' is the header node of a tree.
   //!
   //! <b>Effects</b>: Returns the root of the tree if any, header otherwise
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   inline static NodePtr root_node(ConstNodePtr header) BOOST_NOEXCEPT
   {
      NodePtr p = NodeTraits::get_parent(header);
      return p ? p : detail::uncast(header);
   }

   //! <b>Requires</b>: 'n' is a node of the tree or a node initialized
   //!   by init(...) or init_node.
   //!
   //! <b>Effects</b>: Returns true if the node is initialized by init() or init_node().
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool unique(ConstNodePtr n) BOOST_NOEXCEPT
   { return !TNodeTraits::get_parent(n); }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Requires</b>: 'n' is a node of the tree or a header node.
   //!
   //! <b>Effects</b>: Returns the header of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   static node_ptr get_header(const_node_ptr n);
   #endif

   //! <b>Requires</b>: node1 and node2 can't be header nodes
   //!  of two trees.
   //!
   //! <b>Effects</b>: Swaps two nodes. After the function node1 will be inserted
   //!   in the position node2 before the function. node2 will be inserted in the
   //!   position node1 had before the function.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   node1 and node2 are not equivalent according to the ordering rules.
   //!
   //!Experimental function
   static void swap_nodes(NodePtr node1, NodePtr node2) BOOST_NOEXCEPT
   {
      if(node1 == node2)
         return;

      NodePtr header1(BaseType::get_header(node1)), header2(BaseType::get_header(node2));
      swap_nodes(node1, header1, node2, header2);
   }

   //! <b>Requires</b>: node1 and node2 can't be header nodes
   //!  of two trees with header header1 and header2.
   //!
   //! <b>Effects</b>: Swaps two nodes. After the function node1 will be inserted
   //!   in the position node2 before the function. node2 will be inserted in the
   //!   position node1 had before the function.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   node1 and node2 are not equivalent according to the ordering rules.
   //!
   //!Experimental function
   static void swap_nodes(NodePtr node1, NodePtr header1, NodePtr node2, NodePtr header2) BOOST_NOEXCEPT
   {
      if(node1 == node2)
         return;

      //node1 and node2 must not be header nodes
      //BOOST_INTRUSIVE_INVARIANT_ASSERT((header1 != node1 && header2 != node2));
      if(header1 != header2){
         //Update header1 if necessary
         if(node1 == TNodeTraits::get_left(header1)){
            TNodeTraits::set_left(header1, node2);
         }

         if(node1 == TNodeTraits::get_right(header1)){
            TNodeTraits::set_right(header1, node2);
         }

         if(node1 == TNodeTraits::get_parent(header1)){
            TNodeTraits::set_parent(header1, node2);
         }

         //Update header2 if necessary
         if(node2 == TNodeTraits::get_left(header2)){
            TNodeTraits::set_left(header2, node1);
         }

         if(node2 == TNodeTraits::get_right(header2)){
            TNodeTraits::set_right(header2, node1);
         }

         if(node2 == TNodeTraits::get_parent(header2)){
            TNodeTraits::set_parent(header2, node1);
         }
      }
      else{
         //If both nodes are from the same tree
         //Update header if necessary
         if(node1 == TNodeTraits::get_left(header1)){
            TNodeTraits::set_left(header1, node2);
         }
         else if(node2 == TNodeTraits::get_left(header2)){
            TNodeTraits::set_left(header2, node1);
         }

         if(node1 == TNodeTraits::get_right(header1)){
            TNodeTraits::set_right(header1, node2);
         }
         else if(node2 == TNodeTraits::get_right(header2)){
            TNodeTraits::set_right(header2, node1);
         }

         if(node1 == TNodeTraits::get_parent(header1)){
            TNodeTraits::set_parent(header1, node2);
         }
         else if(node2 == TNodeTraits::get_parent(header2)){
            TNodeTraits::set_parent(header2, node1);
         }

         //Adjust data in nodes to be swapped
         //so that final link swap works as expected
         if(node1 == TNodeTraits::get_parent(node2)){
            TNodeTraits::set_parent(node2, node2);

            if(node2 == TNodeTraits::get_right(node1)){
               TNodeTraits::set_right(node1, node1);
            }
            else{
               TNodeTraits::set_left(node1, node1);
            }
         }
         else if(node2 == TNodeTraits::get_parent(node1)){
            TNodeTraits::set_parent(node1, node1);

            if(node1 == TNodeTraits::get_right(node2)){
               TNodeTraits::set_right(node2, node2);
            }
            else{
               TNodeTraits::set_left(node2, node2);
            }
         }
      }

      //Now swap all the links
      NodePtr temp;
      //swap left link
      temp = TNodeTraits::get_left(node1);
      TNodeTraits::set_left(node1, TNodeTraits::get_left(node2));
      TNodeTraits::set_left(node2, temp);
      //swap right link
      temp = TNodeTraits::get_right(node1);
      TNodeTraits::set_right(node1, TNodeTraits::get_right(node2));
      TNodeTraits::set_right(node2, temp);
      //swap parent link
      temp = TNodeTraits::get_parent(node1);
      TNodeTraits::set_parent(node1, TNodeTraits::get_parent(node2));
      TNodeTraits::set_parent(node2, temp);

      //Now adjust child nodes for newly inserted node 1
      if((temp = TNodeTraits::get_left(node1))){
         TNodeTraits::set_parent(temp, node1);
      }
      if((temp = TNodeTraits::get_right(node1))){
         TNodeTraits::set_parent(temp, node1);
      }
      //Now adjust child nodes for newly inserted node 2
      if((temp = TNodeTraits::get_left(node2))){
         TNodeTraits::set_parent(temp, node2);
      }
      if((temp = TNodeTraits::get_right(node2))){
         TNodeTraits::set_parent(temp, node2);
      }

      //Finally adjust parent nodes
      if ((temp = TNodeTraits::get_parent(node1)) == TNodeTraits::get_parent(node2)) {
         // special logic for the case where the nodes are siblings
         const NodePtr left = TNodeTraits::get_left(temp);
         TNodeTraits::set_left(temp, TNodeTraits::get_right(temp));
         TNodeTraits::set_right(temp, left);
      } else {
         if ((temp = TNodeTraits::get_parent(node1)) &&
            //The header has been already updated so avoid it
            temp != header2) {
            if (TNodeTraits::get_left(temp) == node2) {
               TNodeTraits::set_left(temp, node1);
            }
            if (TNodeTraits::get_right(temp) == node2) {
               TNodeTraits::set_right(temp, node1);
            }
         }
         if ((temp = TNodeTraits::get_parent(node2)) &&
            //The header has been already updated so avoid it
            temp != header1) {
            if (TNodeTraits::get_left(temp) == node1) {
               TNodeTraits::set_left(temp, node2);
            }
            if (TNodeTraits::get_right(temp) == node1) {
               TNodeTraits::set_right(temp, node2);
            }
         }
      }
   }

   //! <b>Requires</b>: node_to_be_replaced must be inserted in a tree
   //!   and new_node must not be inserted in a tree.
   //!
   //! <b>Effects</b>: Replaces node_to_be_replaced in its position in the
   //!   tree with new_node. The tree does not need to be rebalanced
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   new_node is not equivalent to node_to_be_replaced according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing and comparison is needed. Experimental function
   inline static void replace_node(NodePtr node_to_be_replaced, NodePtr new_node) BOOST_NOEXCEPT
   {
      replace_node(node_to_be_replaced, BaseType::get_header(node_to_be_replaced), new_node);
   }

   //! <b>Requires</b>: node_to_be_replaced must be inserted in a tree
   //!   with header "header" and new_node must not be inserted in a tree.
   //!
   //! <b>Effects</b>: Replaces node_to_be_replaced in its position in the
   //!   tree with new_node. The tree does not need to be rebalanced
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This function will break container ordering invariants if
   //!   new_node is not equivalent to node_to_be_replaced according to the
   //!   ordering rules. This function is faster than erasing and inserting
   //!   the node, since no rebalancing or comparison is needed. Experimental function
   static void replace_node(NodePtr node_to_be_replaced, NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BOOST_ASSERT(node_to_be_replaced != new_node);

      //Update header if necessary
      if(node_to_be_replaced == TNodeTraits::get_left(header)){
         TNodeTraits::set_left(header, new_node);
      }

      if(node_to_be_replaced == TNodeTraits::get_right(header)){
         TNodeTraits::set_right(header, new_node);
      }

      if(node_to_be_replaced == TNodeTraits::get_parent(header)){
         TNodeTraits::set_parent(header, new_node);
      }

      //Now set data from the original node
      NodePtr temp;
      TNodeTraits::set_left(new_node, TNodeTraits::get_left(node_to_be_replaced));
      TNodeTraits::set_right(new_node, TNodeTraits::get_right(node_to_be_replaced));
      TNodeTraits::set_parent(new_node, TNodeTraits::get_parent(node_to_be_replaced));

      //Now adjust adjacent nodes for newly inserted node
      if((temp = TNodeTraits::get_left(new_node))){
         TNodeTraits::set_parent(temp, new_node);
      }
      if((temp = TNodeTraits::get_right(new_node))){
         TNodeTraits::set_parent(temp, new_node);
      }
      if((temp = TNodeTraits::get_parent(new_node)) &&
         //The header has been already updated so avoid it
         temp != header){
         if(TNodeTraits::get_left(temp) == node_to_be_replaced){
            TNodeTraits::set_left(temp, new_node);
         }
         if(TNodeTraits::get_right(temp) == node_to_be_replaced){
            TNodeTraits::set_right(temp, new_node);
         }
      }
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Requires</b>: 'n' is a node from the tree except the header.
   //!
   //! <b>Effects</b>: Returns the next node of the tree.
   //!
   //! <b>Complexity</b>: Average constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static node_ptr next_node(node_ptr n) BOOST_NOEXCEPT;

   //! <b>Requires</b>: 'n' is a node from the tree except the leftmost node.
   //!
   //! <b>Effects</b>: Returns the previous node of the tree.
   //!
   //! <b>Complexity</b>: Average constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static node_ptr prev_node(node_ptr n) BOOST_NOEXCEPT;

   //! <b>Requires</b>: 'n' is a node of a tree but not the header.
   //!
   //! <b>Effects</b>: Returns the minimum node of the subtree starting at p.
   //!
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   static node_ptr minimum(node_ptr n);

   //! <b>Requires</b>: 'n' is a node of a tree but not the header.
   //!
   //! <b>Effects</b>: Returns the maximum node of the subtree starting at p.
   //!
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   static node_ptr maximum(node_ptr n);
   #endif

   //! <b>Requires</b>: 'n' must not be part of any tree.
   //!
   //! <b>Effects</b>: After the function unique(node) == true.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If node is inserted in a tree, this function corrupts the tree.
   static void init(NodePtr n) BOOST_NOEXCEPT
   {
      TNodeTraits::set_parent(n, NodePtr());
      TNodeTraits::set_left(n, NodePtr());
      TNodeTraits::set_right(n, NodePtr());
   }

   //! <b>Effects</b>: Returns true if node is in the same state as if called init(node)
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   static bool inited(ConstNodePtr n)
   {
      return !TNodeTraits::get_parent(n) &&
             !TNodeTraits::get_left(n)   &&
             !TNodeTraits::get_right(n)  ;
   }

   //! <b>Requires</b>: header must not be part of any tree.
   //!
   //! <b>Effects</b>: Initializes the header to represent an empty tree.
   //!   unique(header) == true.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Nodes</b>: If header is inserted in a tree, this function corrupts the tree.
   static void init_header(NodePtr header) BOOST_NOEXCEPT
   {
      TNodeTraits::set_parent(header, NodePtr());
      TNodeTraits::set_left(header, header);
      TNodeTraits::set_right(header, header);
   }

   //! <b>Requires</b>: "disposer" must be an object function
   //!   taking a node_ptr parameter and shouldn't throw.
   //!
   //! <b>Effects</b>: Empties the target tree calling
   //!   <tt>void disposer::operator()(node_ptr)</tt> for every node of the tree
   //!    except the header.
   //!
   //! <b>Complexity</b>: Linear to the number of element of the source tree plus the.
   //!   number of elements of tree target tree when calling this function.
   //!
   //! <b>Throws</b>: Nothing.
   template<class Disposer>
   static void clear_and_dispose(NodePtr header, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr source_root = TNodeTraits::get_parent(header);
      if(!source_root)
         return;
      dispose_subtree(source_root, disposer);
      init_header(header);
   }

   //! <b>Requires</b>: header is the header of a tree.
   //!
   //! <b>Effects</b>: Unlinks the leftmost node from the tree, and
   //!   updates the header link to the new leftmost node.
   //!
   //! <b>Complexity</b>: Average complexity is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function breaks the tree and the tree can
   //!   only be used for more unlink_leftmost_without_rebalance calls.
   //!   This function is normally used to achieve a step by step
   //!   controlled destruction of the tree.
   static NodePtr unlink_leftmost_without_rebalance(NodePtr header) BOOST_NOEXCEPT
   {
      NodePtr leftmost = TNodeTraits::get_left(header);
      if (leftmost == header)
         return NodePtr();
      NodePtr leftmost_parent(TNodeTraits::get_parent(leftmost));
      NodePtr leftmost_right (TNodeTraits::get_right(leftmost));
      bool is_root = leftmost_parent == header;

      if (leftmost_right){
         TNodeTraits::set_parent(leftmost_right, leftmost_parent);
         TNodeTraits::set_left(header, BaseType::minimum(leftmost_right));

         if (is_root)
            TNodeTraits::set_parent(header, leftmost_right);
         else
            TNodeTraits::set_left(TNodeTraits::get_parent(header), leftmost_right);
      }
      else if (is_root){
         TNodeTraits::set_parent(header, NodePtr());
         TNodeTraits::set_left(header,  header);
         TNodeTraits::set_right(header, header);
      }
      else{
         TNodeTraits::set_left(leftmost_parent, NodePtr());
         TNodeTraits::set_left(header, leftmost_parent);
      }
      return leftmost;
   }

   //! <b>Requires</b>: 'header' the header of the tree.
   //!
   //! <b>Effects</b>: Returns the number of nodes of the tree.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Throws</b>: Nothing.
   static std::size_t size(ConstNodePtr header) BOOST_NOEXCEPT
   {
      NodePtr beg(begin_node(header));
      NodePtr end(end_node(header));
      std::size_t i = 0;
      for(;beg != end; beg = BaseType::next_node(beg)) ++i;
      return i;
   }

   //! <b>Requires</b>: header1 and header2 must be the header nodes
   //!  of two trees.
   //!
   //! <b>Effects</b>: Swaps two trees. After the function header1 will contain
   //!   links to the second tree and header2 will have links to the first tree.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   static void swap_tree(NodePtr header1, NodePtr header2) BOOST_NOEXCEPT
   {
      if(header1 == header2)
         return;

      NodePtr tmp;

      //Parent swap
      tmp = TNodeTraits::get_parent(header1);
      TNodeTraits::set_parent(header1, TNodeTraits::get_parent(header2));
      TNodeTraits::set_parent(header2, tmp);
      //Left swap
      tmp = TNodeTraits::get_left(header1);
      TNodeTraits::set_left(header1, TNodeTraits::get_left(header2));
      TNodeTraits::set_left(header2, tmp);
      //Right swap
      tmp = TNodeTraits::get_right(header1);
      TNodeTraits::set_right(header1, TNodeTraits::get_right(header2));
      TNodeTraits::set_right(header2, tmp);

      //Now test parent
      NodePtr h1_parent(TNodeTraits::get_parent(header1));
      if(h1_parent){
         TNodeTraits::set_parent(h1_parent, header1);
      }
      else{
         TNodeTraits::set_left(header1, header1);
         TNodeTraits::set_right(header1, header1);
      }

      NodePtr h2_parent(TNodeTraits::get_parent(header2));
      if(h2_parent){
         TNodeTraits::set_parent(h2_parent, header2);
      }
      else{
         TNodeTraits::set_left(header2, header2);
         TNodeTraits::set_right(header2, header2);
      }
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Requires</b>: p is a node of a tree.
   //!
   //! <b>Effects</b>: Returns true if p is the header of the tree.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   static bool is_header(const_node_ptr p) BOOST_NOEXCEPT;
   #endif

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns a node_ptr to the first element that is equivalent to
   //!   "key" according to "comp" or "header" if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr find
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      NodePtr end = detail::uncast(header);
      NodePtr y = lower_bound(header, key, comp);
      return (y == end || comp(key, y)) ? end : y;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!   'lower_key' must not be greater than 'upper_key' according to 'comp'. If
   //!   'lower_key' == 'upper_key', ('left_closed' || 'right_closed') must be true.
   //!
   //! <b>Effects</b>: Returns an a pair with the following criteria:
   //!
   //!   first = lower_bound(lower_key) if left_closed, upper_bound(lower_key) otherwise
   //!
   //!   second = upper_bound(upper_key) if right_closed, lower_bound(upper_key) otherwise
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   //!
   //! <b>Note</b>: This function can be more efficient than calling upper_bound
   //!   and lower_bound for lower_key and upper_key.
   //!
   //! <b>Note</b>: Experimental function, the interface might change.
   template< class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> bounded_range
      ( ConstNodePtr header
      , const KeyType &lower_key
      , const KeyType &upper_key
      , KeyNodePtrCompare comp
      , bool left_closed
      , bool right_closed)
   {
      NodePtr y = detail::uncast(header);
      NodePtr x = TNodeTraits::get_parent(header);

      while(x){
         //If x is less than lower_key the target
         //range is on the right part
         if(comp(x, lower_key)){
            //Check for invalid input range
            BOOST_INTRUSIVE_INVARIANT_ASSERT(comp(x, upper_key));
            x = TNodeTraits::get_right(x);
         }
         //If the upper_key is less than x, the target
         //range is on the left part
         else if(comp(upper_key, x)){
            y = x;
            x = TNodeTraits::get_left(x);
         }
         else{
            //x is inside the bounded range(lower_key <= x <= upper_key),
            //so we must split lower and upper searches
            //
            //Sanity check: if lower_key and upper_key are equal, then both left_closed and right_closed can't be false
            BOOST_INTRUSIVE_INVARIANT_ASSERT(left_closed || right_closed || comp(lower_key, x) || comp(x, upper_key));
            return std::pair<NodePtr,NodePtr>(
               left_closed
                  //If left_closed, then comp(x, lower_key) is already the lower_bound
                  //condition so we save one comparison and go to the next level
                  //following traditional lower_bound algo
                  ? lower_bound_loop(TNodeTraits::get_left(x), x, lower_key, comp)
                  //If left-open, comp(x, lower_key) is not the upper_bound algo
                  //condition so we must recheck current 'x' node with upper_bound algo
                  : upper_bound_loop(x, y, lower_key, comp)
            ,
               right_closed
                  //If right_closed, then comp(upper_key, x) is already the upper_bound
                  //condition so we can save one comparison and go to the next level
                  //following lower_bound algo
                  ? upper_bound_loop(TNodeTraits::get_right(x), y, upper_key, comp)
                  //If right-open, comp(upper_key, x) is not the lower_bound algo
                  //condition so we must recheck current 'x' node with lower_bound algo
                  : lower_bound_loop(x, y, upper_key, comp)
            );
         }
      }
      return std::pair<NodePtr,NodePtr> (y, y);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns the number of elements with a key equivalent to "key"
   //!   according to "comp".
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      std::pair<NodePtr, NodePtr> ret = equal_range(header, key, comp);
      std::size_t n = 0;
      while(ret.first != ret.second){
         ++n;
         ret.first = BaseType::next_node(ret.first);
      }
      return n;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an a pair of node_ptr delimiting a range containing
   //!   all elements that are equivalent to "key" according to "comp" or an
   //!   empty range that indicates the position where those elements would be
   //!   if there are no equivalent elements.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   inline static std::pair<NodePtr, NodePtr> equal_range
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      return bounded_range(header, key, key, comp, true, true);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns an a pair of node_ptr delimiting a range containing
   //!   the first element that is equivalent to "key" according to "comp" or an
   //!   empty range that indicates the position where that element would be
   //!   if there are no equivalent elements.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> lower_bound_range
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      NodePtr const lb(lower_bound(header, key, comp));
      std::pair<NodePtr, NodePtr> ret_ii(lb, lb);
      if(lb != header && !comp(key, lb)){
         ret_ii.second = BaseType::next_node(ret_ii.second);
      }
      return ret_ii;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns a node_ptr to the first element that is
   //!   not less than "key" according to "comp" or "header" if that element does
   //!   not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   inline static NodePtr lower_bound
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      return lower_bound_loop(TNodeTraits::get_parent(header), detail::uncast(header), key, comp);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. KeyNodePtrCompare can compare KeyType with tree's node_ptrs.
   //!
   //! <b>Effects</b>: Returns a node_ptr to the first element that is greater
   //!   than "key" according to "comp" or "header" if that element does not exist.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class KeyType, class KeyNodePtrCompare>
   inline static NodePtr upper_bound
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      return upper_bound_loop(TNodeTraits::get_parent(header), detail::uncast(header), key, comp);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "commit_data" must have been obtained from a previous call to
   //!   "insert_unique_check". No objects should have been inserted or erased
   //!   from the set between the "insert_unique_check" that filled "commit_data"
   //!   and the call to "insert_commit".
   //!
   //!
   //! <b>Effects</b>: Inserts new_node in the set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_unique_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   inline static void insert_unique_commit
      (NodePtr header, NodePtr new_value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return insert_commit(header, new_value, commit_data); }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares KeyType with a node_ptr.
   //!
   //! <b>Effects</b>: Checks if there is an equivalent node to "key" in the
   //!   tree according to "comp" and obtains the needed information to realize
   //!   a constant-time node insertion if there is no equivalent node.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing a node_ptr to the already present node
   //!   and false. If there is not equivalent key can be inserted returns true
   //!   in the returned pair's boolean and fills "commit_data" that is meant to
   //!   be used with the "insert_commit" function to achieve a constant-time
   //!   insertion function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a node is expensive and the user does not want to have two equivalent nodes
   //!   in the tree: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the node and this function offers the possibility to use that part
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the node and use
   //!   "insert_commit" to insert the node in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_unique_commit" only
   //!   if no more objects are inserted or erased from the set.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (ConstNodePtr header, const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      std::size_t depth = 0;
      NodePtr h(detail::uncast(header));
      NodePtr y(h);
      NodePtr x(TNodeTraits::get_parent(y));
      NodePtr prev = NodePtr();

      //Find the upper bound, cache the previous value and if we should
      //store it in the left or right node
      bool left_child = true;
      while(x){
         ++depth;
         y = x;
         left_child = comp(key, x);
         x = left_child ?
               TNodeTraits::get_left(x) : (prev = y, TNodeTraits::get_right(x));
      }

      if(pdepth)  *pdepth = depth;

      //Since we've found the upper bound there is no other value with the same key if:
      //    - There is no previous node
      //    - The previous node is less than the key
      const bool not_present = !prev || comp(prev, key);
      if(not_present){
         commit_data.link_left = left_child;
         commit_data.node      = y;
      }
      return std::pair<NodePtr, bool>(prev, not_present);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   KeyNodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares KeyType with a node_ptr.
   //!   "hint" is node from the "header"'s tree.
   //!
   //! <b>Effects</b>: Checks if there is an equivalent node to "key" in the
   //!   tree according to "comp" using "hint" as a hint to where it should be
   //!   inserted and obtains the needed information to realize
   //!   a constant-time node insertion if there is no equivalent node.
   //!   If "hint" is the upper_bound the function has constant time
   //!   complexity (two comparisons in the worst case).
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing a node_ptr to the already present node
   //!   and false. If there is not equivalent key can be inserted returns true
   //!   in the returned pair's boolean and fills "commit_data" that is meant to
   //!   be used with the "insert_commit" function to achieve a constant-time
   //!   insertion function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic, but it is
   //!   amortized constant time if new_node should be inserted immediately before "hint".
   //!
   //! <b>Throws</b>: If "comp" throws.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a node is expensive and the user does not want to have two equivalent nodes
   //!   in the tree: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the node and this function offers the possibility to use that part
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the node and use
   //!   "insert_commit" to insert the node in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_unique_commit" only
   //!   if no more objects are inserted or erased from the set.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (ConstNodePtr header, NodePtr hint, const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      //hint must be bigger than the key
      if(hint == header || comp(key, hint)){
         NodePtr prev(hint);
         //Previous value should be less than the key
         if(hint == begin_node(header) || comp((prev = BaseType::prev_node(hint)), key)){
            commit_data.link_left = unique(header) || !TNodeTraits::get_left(hint);
            commit_data.node      = commit_data.link_left ? hint : prev;
            if(pdepth){
               *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
            }
            return std::pair<NodePtr, bool>(NodePtr(), true);
         }
      }
      //Hint was wrong, use hintless insertion
      return insert_unique_check(header, key, comp, commit_data, pdepth);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs. "hint" is node from
   //!   the "header"'s tree.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case).
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if new_node is inserted immediately before "hint".
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static NodePtr insert_equal
      (NodePtr h, NodePtr hint, NodePtr new_node, NodePtrCompare comp
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      InsertCommitData commit_data;
      insert_equal_check(h, hint, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the upper bound
   //!   according to "comp".
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static NodePtr insert_equal_upper_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      InsertCommitData commit_data;
      insert_equal_upper_bound_check(h, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the lower bound
   //!   according to "comp".
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare>
   static NodePtr insert_equal_lower_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      InsertCommitData commit_data;
      insert_equal_lower_bound_check(h, new_node, comp, commit_data, pdepth);
      insert_commit(h, new_node, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "pos" must be a valid iterator or header (end) node.
   //!   "pos" must be an iterator pointing to the successor to "new_node"
   //!   once inserted according to the order of already inserted nodes. This function does not
   //!   check "pos" and this precondition must be guaranteed by the caller.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: If "pos" is not the successor of the newly inserted "new_node"
   //! tree invariants might be broken.
   static NodePtr insert_before
      (NodePtr header, NodePtr pos, NodePtr new_node
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      ) BOOST_NOEXCEPT
   {
      InsertCommitData commit_data;
      insert_before_check(header, pos, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering no less than the
   //!   greatest inserted key.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: If "new_node" is less than the greatest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   static void push_back
      (NodePtr header, NodePtr new_node
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      ) BOOST_NOEXCEPT
   {
      InsertCommitData commit_data;
      push_back_check(header, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering, no greater than the
   //!   lowest inserted key.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before "pos".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: If "new_node" is greater than the lowest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   static void push_front
      (NodePtr header, NodePtr new_node
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      ) BOOST_NOEXCEPT
   {
      InsertCommitData commit_data;
      push_front_check(header, commit_data, pdepth);
      insert_commit(header, new_node, commit_data);
   }

   //! <b>Requires</b>: 'n' can't be a header node.
   //!
   //! <b>Effects</b>: Calculates the depth of a node: the depth of a
   //! node is the length (number of edges) of the path from the root
   //! to that node. (The root node is at depth 0.)
   //!
   //! <b>Complexity</b>: Logarithmic to the number of nodes in the tree.
   //!
   //! <b>Throws</b>: Nothing.
   static std::size_t depth(ConstNodePtr n) BOOST_NOEXCEPT
   {
      std::size_t depth = 0;
      NodePtr p_parent;
      while(n != TNodeTraits::get_parent(p_parent = TNodeTraits::get_parent(n))){
         ++depth;
         n = p_parent;
      }
      return depth;
   }

   //! <b>Requires</b>: "cloner" must be a function
   //!   object taking a node_ptr and returning a new cloned node of it. "disposer" must
   //!   take a node_ptr and shouldn't throw.
   //!
   //! <b>Effects</b>: First empties target tree calling
   //!   <tt>void disposer::operator()(node_ptr)</tt> for every node of the tree
   //!    except the header.
   //!
   //!   Then, duplicates the entire tree pointed by "source_header" cloning each
   //!   source node with <tt>node_ptr Cloner::operator()(node_ptr)</tt> to obtain
   //!   the nodes of the target tree. If "cloner" throws, the cloned target nodes
   //!   are disposed using <tt>void disposer(node_ptr )</tt>.
   //!
   //! <b>Complexity</b>: Linear to the number of element of the source tree plus the
   //!   number of elements of tree target tree when calling this function.
   //!
   //! <b>Throws</b>: If cloner functor throws. If this happens target nodes are disposed.
   template <class Cloner, class Disposer>
   static void clone
      (ConstNodePtr source_header, NodePtr target_header, Cloner cloner, Disposer disposer)
   {
      if(!unique(target_header)){
         clear_and_dispose(target_header, disposer);
      }

      NodePtr leftmost, rightmost;
      NodePtr new_root = clone_subtree
         (source_header, target_header, cloner, disposer, leftmost, rightmost);

      //Now update header node
      TNodeTraits::set_parent(target_header, new_root);
      TNodeTraits::set_left  (target_header, leftmost);
      TNodeTraits::set_right (target_header, rightmost);
   }

   //! <b>Requires</b>: header must be the header of a tree, z a node
   //!    of that tree and z != header.
   //!
   //! <b>Effects</b>: Erases node "z" from the tree with header "header".
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!
   //! <b>Throws</b>: Nothing.
   inline static void erase(NodePtr header, NodePtr z) BOOST_NOEXCEPT
   {
      DataForRebalance ignored;
      erase(header, z, ignored);
   }

   //! <b>Requires</b>: header1 and header2 must be the headers of trees tree1 and tree2
   //!   respectively, z a non-header node of tree1. NodePtrCompare is the comparison
   //!   function of tree1..
   //!
   //! <b>Effects</b>: Transfers node "z" from tree1 to tree2 if tree1 does not contain
   //!   a node that is equivalent to z.
   //!
   //! <b>Returns</b>: True if the node was trasferred, false otherwise.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If the comparison throws.
   template<class NodePtrCompare>
   inline static bool transfer_unique
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      DataForRebalance ignored;
      return transfer_unique(header1, comp, header2, z, ignored);
   }

   //! <b>Requires</b>: header1 and header2 must be the headers of trees tree1 and tree2
   //!   respectively, z a non-header node of tree1. NodePtrCompare is the comparison
   //!   function of tree1..
   //!
   //! <b>Effects</b>: Transfers node "z" from tree1 to tree2.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: If the comparison throws.
   template<class NodePtrCompare>
   inline static void transfer_equal
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      DataForRebalance ignored;
      transfer_equal(header1, comp, header2, z, ignored);
   }

   //! <b>Requires</b>: 'n' is a tree node but not the header.
   //!
   //! <b>Effects</b>: Unlinks the node and rebalances the tree.
   //!
   //! <b>Complexity</b>: Average complexity is constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static void unlink(NodePtr n) BOOST_NOEXCEPT
   {
      NodePtr x = TNodeTraits::get_parent(n);
      if(x){
         while(!BaseType::is_header(x))
            x = TNodeTraits::get_parent(x);
         erase(x, n);
      }
   }

   //! <b>Requires</b>: header must be the header of a tree.
   //!
   //! <b>Effects</b>: Rebalances the tree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear.
   static void rebalance(NodePtr header) BOOST_NOEXCEPT
   {
      NodePtr root = TNodeTraits::get_parent(header);
      if(root){
         rebalance_subtree(root);
      }
   }

   //! <b>Requires</b>: old_root is a node of a tree. It shall not be null.
   //!
   //! <b>Effects</b>: Rebalances the subtree rooted at old_root.
   //!
   //! <b>Returns</b>: The new root of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear.
   static NodePtr rebalance_subtree(NodePtr old_root) BOOST_NOEXCEPT
   {
      //Taken from:
      //"Tree rebalancing in optimal time and space"
      //Quentin F. Stout and Bette L. Warren

      //To avoid irregularities in the algorithm (old_root can be a
      //left or right child or even the root of the tree) just put the
      //root as the right child of its parent. Before doing this backup
      //information to restore the original relationship after
      //the algorithm is applied.
      NodePtr super_root = TNodeTraits::get_parent(old_root);
      BOOST_INTRUSIVE_INVARIANT_ASSERT(super_root);

      //Get root info
      NodePtr super_root_right_backup = TNodeTraits::get_right(super_root);
      bool super_root_is_header = TNodeTraits::get_parent(super_root) == old_root;
      bool old_root_is_right  = is_right_child(old_root);
      TNodeTraits::set_right(super_root, old_root);

      std::size_t size;
      subtree_to_vine(super_root, size);
      vine_to_subtree(super_root, size);
      NodePtr new_root = TNodeTraits::get_right(super_root);

      //Recover root
      if(super_root_is_header){
         TNodeTraits::set_right(super_root, super_root_right_backup);
         TNodeTraits::set_parent(super_root, new_root);
      }
      else if(old_root_is_right){
         TNodeTraits::set_right(super_root, new_root);
      }
      else{
         TNodeTraits::set_right(super_root, super_root_right_backup);
         TNodeTraits::set_left(super_root, new_root);
      }
      return new_root;
   }

   //! <b>Effects</b>: Asserts the integrity of the container with additional checks provided by the user.
   //!
   //! <b>Requires</b>: header must be the header of a tree.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Note</b>: The method might not have effect when asserts are turned off (e.g., with NDEBUG).
   //!   Experimental function, interface might change in future versions.
   template<class Checker>
   static void check(ConstNodePtr header, Checker checker, typename Checker::return_type& checker_return)
   {
      ConstNodePtr root_node_ptr = TNodeTraits::get_parent(header);
      if (!root_node_ptr){
         // check left&right header pointers
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_left(header) == header);
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_right(header) == header);
      }
      else{
         // check parent pointer of root node
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_parent(root_node_ptr) == header);
         // check subtree from root
         check_subtree(root_node_ptr, checker, checker_return);
         // check left&right header pointers
         ConstNodePtr p = root_node_ptr;
         while (TNodeTraits::get_left(p)) { p = TNodeTraits::get_left(p); }
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_left(header) == p);
         p = root_node_ptr;
         while (TNodeTraits::get_right(p)) { p = TNodeTraits::get_right(p); }
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_right(header) == p);
      }
   }

   protected:

   template<class NodePtrCompare>
   static bool transfer_unique
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z, DataForRebalance &info)
   {
      InsertCommitData commit_data;
      bool const transferable = insert_unique_check(header1, z, comp, commit_data).second;
      if(transferable){
         erase(header2, z, info);
         insert_commit(header1, z, commit_data);
      }
      return transferable;
   }

   template<class NodePtrCompare>
   static void transfer_equal
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z, DataForRebalance &info)
   {
      InsertCommitData commit_data;
      insert_equal_upper_bound_check(header1, z, comp, commit_data);
      erase(header2, z, info);
      insert_commit(header1, z, commit_data);
   }

   static void erase(NodePtr header, NodePtr z, DataForRebalance &info)
   {
      NodePtr y(z);
      NodePtr x;
      const NodePtr z_left(TNodeTraits::get_left(z));
      const NodePtr z_right(TNodeTraits::get_right(z));

      if(!z_left){
         x = z_right;    // x might be null.
      }
      else if(!z_right){ // z has exactly one non-null child. y == z.
         x = z_left;       // x is not null.
         BOOST_ASSERT(x);
      }
      else{ //make y != z
         // y = find z's successor
         y = BaseType::minimum(z_right);
         x = TNodeTraits::get_right(y);     // x might be null.
      }

      NodePtr x_parent;
      const NodePtr z_parent(TNodeTraits::get_parent(z));
      const bool z_is_leftchild(TNodeTraits::get_left(z_parent) == z);

      if(y != z){ //has two children and y is the minimum of z
         //y is z's successor and it has a null left child.
         //x is the right child of y (it can be null)
         //Relink y in place of z and link x with y's old parent
         TNodeTraits::set_parent(z_left, y);
         TNodeTraits::set_left(y, z_left);
         if(y != z_right){
            //Link y with the right tree of z
            TNodeTraits::set_right(y, z_right);
            TNodeTraits::set_parent(z_right, y);
            //Link x with y's old parent (y must be a left child)
            x_parent = TNodeTraits::get_parent(y);
            BOOST_ASSERT(TNodeTraits::get_left(x_parent) == y);
            if(x)
               TNodeTraits::set_parent(x, x_parent);
            //Since y was the successor and not the right child of z, it must be a left child
            TNodeTraits::set_left(x_parent, x);
         }
         else{ //y was the right child of y so no need to fix x's position
            x_parent = y;
         }
         TNodeTraits::set_parent(y, z_parent);
         ThisType::set_child(header, y, z_parent, z_is_leftchild);
      }
      else {  // z has zero or one child, x is one child (it can be null)
         //Just link x to z's parent
         x_parent = z_parent;
         if(x)
            TNodeTraits::set_parent(x, z_parent);
         ThisType::set_child(header, x, z_parent, z_is_leftchild);

         //Now update leftmost/rightmost in case z was one of them
         if(TNodeTraits::get_left(header) == z){
            //z_left must be null because z is the leftmost
            BOOST_ASSERT(!z_left);
            TNodeTraits::set_left(header, !z_right ?
               z_parent :  // makes leftmost == header if z == root
               BaseType::minimum(z_right));
         }
         if(TNodeTraits::get_right(header) == z){
            //z_right must be null because z is the rightmost
            BOOST_ASSERT(!z_right);
            TNodeTraits::set_right(header, !z_left ?
               z_parent :  // makes rightmost == header if z == root
               BaseType::maximum(z_left));
         }
      }

      //If z had 0/1 child, y == z and one of its children (and maybe null)
      //If z had 2 children, y is the successor of z and x is the right child of y
      info.x = x;
      info.y = y;
      //If z had 0/1 child, x_parent is the new parent of the old right child of y (z's successor)
      //If z had 2 children, x_parent is the new parent of y (z_parent)
      BOOST_ASSERT(!x || TNodeTraits::get_parent(x) == x_parent);
      info.x_parent = x_parent;
   }

   //! <b>Requires</b>: 'subtree' is a node of the tree but it's not the header.
   //!
   //! <b>Effects</b>: Returns the number of nodes of the subtree.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Throws</b>: Nothing.
   static std::size_t subtree_size(ConstNodePtr subtree) BOOST_NOEXCEPT
   {
      std::size_t count = 0;
      if (subtree){
         NodePtr n = detail::uncast(subtree);
         NodePtr m = TNodeTraits::get_left(n);
         while(m){
            n = m;
            m = TNodeTraits::get_left(n);
         }

         while(1){
            ++count;
            NodePtr n_right(TNodeTraits::get_right(n));
            if(n_right){
               n = n_right;
               m = TNodeTraits::get_left(n);
               while(m){
                  n = m;
                  m = TNodeTraits::get_left(n);
               }
            }
            else {
               do{
                  if (n == subtree){
                     return count;
                  }
                  m = n;
                  n = TNodeTraits::get_parent(n);
               }while(TNodeTraits::get_left(n) != m);
            }
         }
      }
      return count;
   }

   //! <b>Requires</b>: p is a node of a tree.
   //!
   //! <b>Effects</b>: Returns true if p is a left child.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool is_left_child(NodePtr p) BOOST_NOEXCEPT
   {  return TNodeTraits::get_left(TNodeTraits::get_parent(p)) == p;  }

   //! <b>Requires</b>: p is a node of a tree.
   //!
   //! <b>Effects</b>: Returns true if p is a right child.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool is_right_child(NodePtr p) BOOST_NOEXCEPT
   {  return TNodeTraits::get_right(TNodeTraits::get_parent(p)) == p;  }

   static void insert_before_check
      (NodePtr header, NodePtr pos
      , InsertCommitData &commit_data
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      )
   {
      NodePtr prev(pos);
      if(pos != TNodeTraits::get_left(header))
         prev = BaseType::prev_node(pos);
      bool link_left = unique(header) || !TNodeTraits::get_left(pos);
      commit_data.link_left = link_left;
      commit_data.node = link_left ? pos : prev;
      if(pdepth){
         *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
      }
   }

   static void push_back_check
      (NodePtr header, InsertCommitData &commit_data
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      ) BOOST_NOEXCEPT
   {
      NodePtr prev(TNodeTraits::get_right(header));
      if(pdepth){
         *pdepth = prev == header ? 0 : depth(prev) + 1;
      }
      commit_data.link_left = false;
      commit_data.node = prev;
   }

   static void push_front_check
      (NodePtr header, InsertCommitData &commit_data
         #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
         , std::size_t *pdepth = nullptr
         #endif
      ) BOOST_NOEXCEPT
   {
      NodePtr pos(TNodeTraits::get_left(header));
      if(pdepth){
         *pdepth = pos == header ? 0 : depth(pos) + 1;
      }
      commit_data.link_left = true;
      commit_data.node = pos;
   }

   template<class NodePtrCompare>
   static void insert_equal_check
      (NodePtr header, NodePtr hint, NodePtr new_node, NodePtrCompare comp
      , InsertCommitData &commit_data
      /// @cond
      , std::size_t *pdepth = nullptr
      /// @endcond
      )
   {
      if(hint == header || !comp(hint, new_node)){
         NodePtr prev(hint);
         if(hint == TNodeTraits::get_left(header) ||
            !comp(new_node, (prev = BaseType::prev_node(hint)))){
            bool link_left = unique(header) || !TNodeTraits::get_left(hint);
            commit_data.link_left = link_left;
            commit_data.node = link_left ? hint : prev;
            if(pdepth){
               *pdepth = commit_data.node == header ? 0 : depth(commit_data.node) + 1;
            }
         }
         else{
            insert_equal_upper_bound_check(header, new_node, comp, commit_data, pdepth);
         }
      }
      else{
         insert_equal_lower_bound_check(header, new_node, comp, commit_data, pdepth);
      }
   }

   template<class NodePtrCompare>
   static void insert_equal_upper_bound_check
      (NodePtr h, NodePtr new_node, NodePtrCompare comp, InsertCommitData & commit_data, std::size_t *pdepth = nullptr)
   {
      std::size_t depth = 0;
      NodePtr y(h);
      NodePtr x(TNodeTraits::get_parent(y));

      while(x){
         ++depth;
         y = x;
         x = comp(new_node, x) ?
               TNodeTraits::get_left(x) : TNodeTraits::get_right(x);
      }
      if(pdepth)  *pdepth = depth;
      commit_data.link_left = (y == h) || comp(new_node, y);
      commit_data.node = y;
   }

   template<class NodePtrCompare>
   static void insert_equal_lower_bound_check
      (NodePtr h, NodePtr new_node, NodePtrCompare comp, InsertCommitData & commit_data, std::size_t *pdepth = nullptr)
   {
      std::size_t depth = 0;
      NodePtr y(h);
      NodePtr x(TNodeTraits::get_parent(y));

      while(x){
         ++depth;
         y = x;
         x = !comp(x, new_node) ?
               TNodeTraits::get_left(x) : TNodeTraits::get_right(x);
      }
      if(pdepth)  *pdepth = depth;
      commit_data.link_left = (y == h) || !comp(y, new_node);
      commit_data.node = y;
   }

   static void insert_commit
      (NodePtr header, NodePtr new_node, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      //Check if commit_data has not been initialized by a insert_unique_check call.
      BOOST_INTRUSIVE_INVARIANT_ASSERT(commit_data.node != NodePtr());
      NodePtr parent_node(commit_data.node);
      if(parent_node == header){
         TNodeTraits::set_parent(header, new_node);
         TNodeTraits::set_right(header, new_node);
         TNodeTraits::set_left(header, new_node);
      }
      else if(commit_data.link_left){
         TNodeTraits::set_left(parent_node, new_node);
         if(parent_node == TNodeTraits::get_left(header))
             TNodeTraits::set_left(header, new_node);
      }
      else{
         TNodeTraits::set_right(parent_node, new_node);
         if(parent_node == TNodeTraits::get_right(header))
             TNodeTraits::set_right(header, new_node);
      }
      TNodeTraits::set_parent(new_node, parent_node);
      TNodeTraits::set_right(new_node, NodePtr());
      TNodeTraits::set_left(new_node, NodePtr());
   }

   //Fix header and own's parent data when replacing x with own, providing own's old data with parent
   static void set_child(NodePtr header, NodePtr new_child, NodePtr new_parent, const bool link_left) BOOST_NOEXCEPT
   {
      if(new_parent == header)
         TNodeTraits::set_parent(header, new_child);
      else if(link_left)
         TNodeTraits::set_left(new_parent, new_child);
      else
         TNodeTraits::set_right(new_parent, new_child);
   }

   // rotate p to left (no header and p's parent fixup)
   static void rotate_left_no_parent_fix(NodePtr p, NodePtr p_right) BOOST_NOEXCEPT
   {
      NodePtr p_right_left(TNodeTraits::get_left(p_right));
      TNodeTraits::set_right(p, p_right_left);
      if(p_right_left){
         TNodeTraits::set_parent(p_right_left, p);
      }
      TNodeTraits::set_left(p_right, p);
      TNodeTraits::set_parent(p, p_right);
   }

   // rotate p to left (with header and p's parent fixup)
   static void rotate_left(NodePtr p, NodePtr p_right, NodePtr p_parent, NodePtr header) BOOST_NOEXCEPT
   {
      const bool p_was_left(TNodeTraits::get_left(p_parent) == p);
      rotate_left_no_parent_fix(p, p_right);
      TNodeTraits::set_parent(p_right, p_parent);
      set_child(header, p_right, p_parent, p_was_left);
   }

   // rotate p to right (no header and p's parent fixup)
   static void rotate_right_no_parent_fix(NodePtr p, NodePtr p_left) BOOST_NOEXCEPT
   {
      NodePtr p_left_right(TNodeTraits::get_right(p_left));
      TNodeTraits::set_left(p, p_left_right);
      if(p_left_right){
         TNodeTraits::set_parent(p_left_right, p);
      }
      TNodeTraits::set_right(p_left, p);
      TNodeTraits::set_parent(p, p_left);
   }

   // rotate p to right (with header and p's parent fixup)
   static void rotate_right(NodePtr p, NodePtr p_left, NodePtr p_parent, NodePtr header) BOOST_NOEXCEPT
   {
      const bool p_was_left(TNodeTraits::get_left(p_parent) == p);
      rotate_right_no_parent_fix(p, p_left);
      TNodeTraits::set_parent(p_left, p_parent);
      set_child(header, p_left, p_parent, p_was_left);
   }

   private:

   static void subtree_to_vine(NodePtr vine_tail, std::size_t &size) BOOST_NOEXCEPT
   {
      //Inspired by LibAVL:
      //It uses a clever optimization for trees with parent pointers.
      //No parent pointer is updated when transforming a tree to a vine as
      //most of them will be overriten during compression rotations.
      //A final pass must be made after the rebalancing to updated those
      //pointers not updated by tree_to_vine + compression calls
      std::size_t len = 0;
      NodePtr remainder = TNodeTraits::get_right(vine_tail);
      while(remainder){
         NodePtr tempptr = TNodeTraits::get_left(remainder);
         if(!tempptr){   //move vine-tail down one
            vine_tail = remainder;
            remainder = TNodeTraits::get_right(remainder);
            ++len;
         }
         else{ //rotate
            TNodeTraits::set_left(remainder, TNodeTraits::get_right(tempptr));
            TNodeTraits::set_right(tempptr, remainder);
            remainder = tempptr;
            TNodeTraits::set_right(vine_tail, tempptr);
         }
      }
      size = len;
   }

   static void compress_subtree(NodePtr scanner, std::size_t count) BOOST_NOEXCEPT
   {
      while(count--){   //compress "count" spine nodes in the tree with pseudo-root scanner
         NodePtr child = TNodeTraits::get_right(scanner);
         NodePtr child_right = TNodeTraits::get_right(child);
         TNodeTraits::set_right(scanner, child_right);
         //Avoid setting the parent of child_right
         scanner = child_right;
         NodePtr scanner_left = TNodeTraits::get_left(scanner);
         TNodeTraits::set_right(child, scanner_left);
         if(scanner_left)
            TNodeTraits::set_parent(scanner_left, child);
         TNodeTraits::set_left(scanner, child);
         TNodeTraits::set_parent(child, scanner);
      }
   }

   static void vine_to_subtree(NodePtr super_root, std::size_t count) BOOST_NOEXCEPT
   {
      const std::size_t one_szt = 1u;
      std::size_t leaf_nodes = count + one_szt - std::size_t(one_szt << detail::floor_log2(count + one_szt));
      compress_subtree(super_root, leaf_nodes);  //create deepest leaves
      std::size_t vine_nodes = count - leaf_nodes;
      while(vine_nodes > 1){
         vine_nodes /= 2;
         compress_subtree(super_root, vine_nodes);
      }

      //Update parents of nodes still in the in the original vine line
      //as those have not been updated by subtree_to_vine or compress_subtree
      for ( NodePtr q = super_root, p = TNodeTraits::get_right(super_root)
          ; p
          ; q = p, p = TNodeTraits::get_right(p)){
         TNodeTraits::set_parent(p, q);
      }
   }

   //! <b>Requires</b>: "n" must be a node inserted in a tree.
   //!
   //! <b>Effects</b>: Returns a pointer to the header node of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr get_root(NodePtr n) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT((!inited(n)));
      NodePtr x = TNodeTraits::get_parent(n);
      if(x){
         while(!BaseType::is_header(x)){
            x = TNodeTraits::get_parent(x);
         }
         return x;
      }
      else{
         return n;
      }
   }

   template <class Cloner, class Disposer>
   static NodePtr clone_subtree
      (ConstNodePtr source_parent, NodePtr target_parent
      , Cloner cloner, Disposer disposer
      , NodePtr &leftmost_out, NodePtr &rightmost_out
      )
   {
      NodePtr target_sub_root = target_parent;
      NodePtr source_root = TNodeTraits::get_parent(source_parent);
      if(!source_root){
         leftmost_out = rightmost_out = source_root;
      }
      else{
         //We'll calculate leftmost and rightmost nodes while iterating
         NodePtr current = source_root;
         NodePtr insertion_point = target_sub_root = cloner(current);

         //We'll calculate leftmost and rightmost nodes while iterating
         NodePtr leftmost  = target_sub_root;
         NodePtr rightmost = target_sub_root;

         //First set the subroot
         TNodeTraits::set_left(target_sub_root, NodePtr());
         TNodeTraits::set_right(target_sub_root, NodePtr());
         TNodeTraits::set_parent(target_sub_root, target_parent);

         DisposeSubtreeDisposer<Disposer> rollback(disposer, target_sub_root);
         while(true) {
            //First clone left nodes
            if( TNodeTraits::get_left(current) &&
               !TNodeTraits::get_left(insertion_point)) {
               current = TNodeTraits::get_left(current);
               NodePtr temp = insertion_point;
               //Clone and mark as leaf
               insertion_point = cloner(current);
               TNodeTraits::set_left  (insertion_point, NodePtr());
               TNodeTraits::set_right (insertion_point, NodePtr());
               //Insert left
               TNodeTraits::set_parent(insertion_point, temp);
               TNodeTraits::set_left  (temp, insertion_point);
               //Update leftmost
               if(rightmost == target_sub_root)
                  leftmost = insertion_point;
            }
            //Then clone right nodes
            else if( TNodeTraits::get_right(current) &&
                     !TNodeTraits::get_right(insertion_point)){
               current = TNodeTraits::get_right(current);
               NodePtr temp = insertion_point;
               //Clone and mark as leaf
               insertion_point = cloner(current);
               TNodeTraits::set_left  (insertion_point, NodePtr());
               TNodeTraits::set_right (insertion_point, NodePtr());
               //Insert right
               TNodeTraits::set_parent(insertion_point, temp);
               TNodeTraits::set_right (temp, insertion_point);
               //Update rightmost
               rightmost = insertion_point;
            }
            //If not, go up
            else if(current == source_root){
               break;
            }
            else{
               //Branch completed, go up searching more nodes to clone
               current = TNodeTraits::get_parent(current);
               insertion_point = TNodeTraits::get_parent(insertion_point);
            }
         }
         rollback.release();
         leftmost_out   = leftmost;
         rightmost_out  = rightmost;
      }
      return target_sub_root;
   }

   template<class Disposer>
   static void dispose_subtree(NodePtr x, Disposer disposer) BOOST_NOEXCEPT
   {
      while (x){
         NodePtr save(TNodeTraits::get_left(x));
         if (save) {
            // Right rotation
            TNodeTraits::set_left(x, TNodeTraits::get_right(save));
            TNodeTraits::set_right(save, x);
         }
         else {
            save = TNodeTraits::get_right(x);
            init(x);
            disposer(x);
         }
         x = save;
      }
   }

   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr lower_bound_loop
      (NodePtr x, NodePtr y, const KeyType &key, KeyNodePtrCompare comp)
   {
      while(x){
         if(comp(x, key)){
            x = TNodeTraits::get_right(x);
         }
         else{
            y = x;
            x = TNodeTraits::get_left(x);
         }
      }
      return y;
   }

   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr upper_bound_loop
      (NodePtr x, NodePtr y, const KeyType &key, KeyNodePtrCompare comp)
   {
      while(x){
         if(comp(key, x)){
            y = x;
            x = TNodeTraits::get_left(x);
         }
         else{
            x = TNodeTraits::get_right(x);
         }
      }
      return y;
   }

   template<class Checker>
   static void check_subtree(ConstNodePtr n, Checker checker, typename Checker::return_type& check_return)
   {
      ConstNodePtr left = TNodeTraits::get_left(n);
      ConstNodePtr right = TNodeTraits::get_right(n);
      typename Checker::return_type check_return_left;
      typename Checker::return_type check_return_right;
      if (left)
      {
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_parent(left) == n);
         check_subtree(left, checker, check_return_left);
      }
      if (right)
      {
         BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_parent(right) == n);
         check_subtree(right, checker, check_return_right);
      }
      checker(n, check_return_left, check_return_right, check_return);
   }
};

/// @cond

template<class NodeTraits>
struct get_algo<BsTreeAlgorithmsValue, NodeTraits>
{
   typedef BstreeAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<BsTreeAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef detail::BstreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

}  //namespace intrusive
}  //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_BSTREE_ALGORITHMS_HPP
