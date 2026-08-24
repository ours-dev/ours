/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <cstddef>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//! circular_list_algorithms provides basic algorithms to manipulate nodes
//! forming a circular doubly linked list. An empty circular list is formed by a node
//! whose pointers point to itself.
//!
//! circular_list_algorithms is configured with a NodeTraits class, which encapsulates the
//! information about the node to be manipulated. NodeTraits must support the
//! following interface:
//!
//! <b>Typedefs</b>:
//!
//! <tt>node</tt>: The type of the node that forms the circular list
//!
//! <tt>node_ptr</tt>: A pointer to a node
//!
//! <tt>const_node_ptr</tt>: A pointer to a const node
//!
//! <b>Static functions</b>:
//!
//! <tt>static node_ptr get_previous(const_node_ptr n);</tt>
//!
//! <tt>static void set_previous(node_ptr n, node_ptr prev);</tt>
//!
//! <tt>static node_ptr get_next(const_node_ptr n);</tt>
//!
//! <tt>static void set_next(node_ptr n, node_ptr next);</tt>
template<class TNodeTraits>
class CircularListAlgorithms
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;
   typedef TNodeTraits                           NodeTraits;

   //! <b>Effects</b>: Constructs an non-used list element, so that
   //! inited(this_node) == true
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void init(NodePtr this_node) BOOST_NOEXCEPT
   {
      const NodePtr null_node = NodePtr();
      TNodeTraits::set_next(this_node, null_node);
      TNodeTraits::set_previous(this_node, null_node);
   }

   //! <b>Effects</b>: Returns true is "this_node" is in a non-used state
   //! as if it was initialized by the "init" function.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool inited(ConstNodePtr this_node) BOOST_NOEXCEPT
   {  return !TNodeTraits::get_next(this_node); }

   //! <b>Effects</b>: Constructs an empty list, making this_node the only
   //!   node of the circular list:
   //!  <tt>NodeTraits::get_next(this_node) == NodeTraits::get_previous(this_node)
   //!  == this_node</tt>.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void init_header(NodePtr this_node) BOOST_NOEXCEPT
   {
      TNodeTraits::set_next(this_node, this_node);
      TNodeTraits::set_previous(this_node, this_node);
   }

   //! <b>Effects</b>: Returns true if this_node_points to an empty list.
   //! 
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   inline static bool is_empty(ConstNodePtr this_node) BOOST_NOEXCEPT
   {
      return TNodeTraits::get_next(this_node) == this_node;
   }

   //! <b>Requires</b>: this_node must be in a circular list or be an empty circular list.
   //!
   //! <b>Effects</b>: Returns true is "this_node" is the only node of a circular list:
   //!  <tt>return NodeTraits::get_next(this_node) == this_node</tt>
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static bool unique(ConstNodePtr this_node) BOOST_NOEXCEPT
   {
      NodePtr next = TNodeTraits::get_next(this_node);
      return !next || next == this_node;
   }

   //! <b>Requires</b>: this_node must be in a circular list or be an empty circular list.
   //!
   //! <b>Effects</b>: Returns the number of nodes in a circular list. If the circular list
   //!  is empty, returns 1.
   //!
   //! <b>Complexity</b>: Linear
   //!
   //! <b>Throws</b>: Nothing.
   static std::size_t count(ConstNodePtr this_node) BOOST_NOEXCEPT
   {
      std::size_t result = 0;
      ConstNodePtr p = this_node;
      do{
         p = TNodeTraits::get_next(p);
         ++result;
      }while (p != this_node);
      return result;
   }

   //! <b>Requires</b>: this_node must be in a circular list or be an empty circular list.
   //!
   //! <b>Effects</b>: Unlinks the node from the circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr unlink(NodePtr this_node) BOOST_NOEXCEPT
   {
      NodePtr next(TNodeTraits::get_next(this_node));
      NodePtr prev(TNodeTraits::get_previous(this_node));
      TNodeTraits::set_next(prev, next);
      TNodeTraits::set_previous(next, prev);
      return next;
   }

   //! <b>Requires</b>: b and e must be nodes of the same circular list or an empty range.
   //!
   //! <b>Effects</b>: Unlinks the node [b, e) from the circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void unlink(NodePtr b, NodePtr e) BOOST_NOEXCEPT
   {
      if (b != e) {
         NodePtr prevb(TNodeTraits::get_previous(b));
         TNodeTraits::set_previous(e, prevb);
         TNodeTraits::set_next(prevb, e);
      }
   }

   //! <b>Requires</b>: nxt_node must be a node of a circular list.
   //!
   //! <b>Effects</b>: Links this_node before nxt_node in the circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void link_before(NodePtr nxt_node, NodePtr this_node) BOOST_NOEXCEPT
   {
      NodePtr prev(TNodeTraits::get_previous(nxt_node));
      TNodeTraits::set_previous(this_node, prev);
      TNodeTraits::set_next(this_node, nxt_node);
      //nxt_node might be an alias for prev->next_
      //so use it before NodeTraits::set_next(prev, ...)
      //is called and the reference changes its value
      TNodeTraits::set_previous(nxt_node, this_node);
      TNodeTraits::set_next(prev, this_node);
   }

   //! <b>Requires</b>: prev_node must be a node of a circular list.
   //!
   //! <b>Effects</b>: Links this_node after prev_node in the circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void link_after(NodePtr prev_node, NodePtr this_node) BOOST_NOEXCEPT
   {
      NodePtr next(TNodeTraits::get_next(prev_node));
      TNodeTraits::set_previous(this_node, prev_node);
      TNodeTraits::set_next(this_node, next);
      //prev_node might be an alias for next->next_
      //so use it before update it before NodeTraits::set_previous(next, ...)
      //is called and the reference changes it's value
      TNodeTraits::set_next(prev_node, this_node);
      TNodeTraits::set_previous(next, this_node);
   }

   //! <b>Requires</b>: this_node and other_node must be nodes inserted
   //!  in circular lists or be empty circular lists.
   //!
   //! <b>Effects</b>: Swaps the position of the nodes: this_node is inserted in
   //!   other_nodes position in the second circular list and the other_node is inserted
   //!   in this_node's position in the first circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void swap_nodes(NodePtr this_node, NodePtr other_node) BOOST_NOEXCEPT
   {
      if (other_node == this_node)
         return;
      bool this_inited  = inited(this_node);
      bool other_inited = inited(other_node);
      if(this_inited){
         init_header(this_node);
      }
      if(other_inited){
         init_header(other_node);
      }

      NodePtr next_this(TNodeTraits::get_next(this_node));
      NodePtr prev_this(TNodeTraits::get_previous(this_node));
      NodePtr next_other(TNodeTraits::get_next(other_node));
      NodePtr prev_other(TNodeTraits::get_previous(other_node));
      //these first two swaps must happen before the other two
      swap_prev(next_this, next_other);
      swap_next(prev_this, prev_other);
      swap_next(this_node, other_node);
      swap_prev(this_node, other_node);

      if(this_inited){
         init(other_node);
      }
      if(other_inited){
         init(this_node);
      }
   }

   //! <b>Requires</b>: b and e must be nodes of the same circular list or an empty range.
   //!   and p must be a node of a different circular list or may not be an iterator in
   //    [b, e).
   //!
   //! <b>Effects</b>: Removes the nodes from [b, e) range from their circular list and inserts
   //!   them before p in p's circular list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void transfer(NodePtr p, NodePtr b, NodePtr e) BOOST_NOEXCEPT
   {
      if (b != e && p != b && p != e) {
         NodePtr prev_p(TNodeTraits::get_previous(p));
         NodePtr prev_b(TNodeTraits::get_previous(b));
         NodePtr prev_e(TNodeTraits::get_previous(e));
         TNodeTraits::set_next(prev_e, p);
         TNodeTraits::set_previous(p, prev_e);
         TNodeTraits::set_next(prev_b, e);
         TNodeTraits::set_previous(e, prev_b);
         TNodeTraits::set_next(prev_p, b);
         TNodeTraits::set_previous(b, prev_p);
      }
   }

   //! <b>Requires</b>: i must a node of a circular list
   //!   and p must be a node of a different circular list.
   //!
   //! <b>Effects</b>: Removes the node i from its circular list and inserts
   //!   it before p in p's circular list.
   //!   If p == i or p == NodeTraits::get_next(i), this function is a null operation.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing.
   static void transfer(NodePtr p, NodePtr i) BOOST_NOEXCEPT
   {
      NodePtr n(TNodeTraits::get_next(i));
      if(n != p && i != p){
         NodePtr prev_p(TNodeTraits::get_previous(p));
         NodePtr prev_i(TNodeTraits::get_previous(i));
         TNodeTraits::set_next(prev_p, i);
         TNodeTraits::set_previous(i, prev_p);
         TNodeTraits::set_next(i, p);
         TNodeTraits::set_previous(p, i);
         TNodeTraits::set_previous(n, prev_i);
         TNodeTraits::set_next(prev_i, n);

      }
   }

   //! <b>Effects</b>: Reverses the order of elements in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: This function is linear time.
   static void reverse(NodePtr p) BOOST_NOEXCEPT
   {
      NodePtr f(TNodeTraits::get_next(p));
      NodePtr i(TNodeTraits::get_next(f)), e(p);

      while(i != e) {
         NodePtr n = i;
         i = TNodeTraits::get_next(i);
         transfer(f, n, i);
         f = n;
      }
   }

   //! <b>Effects</b>: Moves the node p n positions towards the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of moved positions.
   static void move_backwards(NodePtr p, std::size_t n) BOOST_NOEXCEPT
   {
      //Null shift, nothing to do
      if(!n) return;
      NodePtr first  = TNodeTraits::get_next(p);
      //size() == 0 or 1, nothing to do
      if(first == TNodeTraits::get_previous(p)) return;
      unlink(p);
      //Now get the new first node
      while(n--){
         first = TNodeTraits::get_next(first);
      }
      link_before(first, p);
   }

   //! <b>Effects</b>: Moves the node p n positions towards the beginning of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of moved positions.
   static void move_forward(NodePtr p, std::size_t n) BOOST_NOEXCEPT
   {
      //Null shift, nothing to do
      if(!n)   return;
      NodePtr last  = TNodeTraits::get_previous(p);
      //size() == 0 or 1, nothing to do
      if(last == TNodeTraits::get_next(p))   return;

      unlink(p);
      //Now get the new last node
      while(n--){
         last = TNodeTraits::get_previous(last);
      }
      link_after(last, p);
   }

   //! <b>Requires</b>: f and l must be in a circular list.
   //!
   //! <b>Effects</b>: Returns the number of nodes in the range [f, l).
   //!
   //! <b>Complexity</b>: Linear
   //!
   //! <b>Throws</b>: Nothing.
   static std::size_t distance(ConstNodePtr f, ConstNodePtr l) BOOST_NOEXCEPT
   {
      std::size_t result = 0;
      while(f != l){
         f = TNodeTraits::get_next(f);
         ++result;
      }
      return result;
   }

   struct StablePartitionInfo
   {
      std::size_t m_num_1st_partition;
      std::size_t m_num_2nd_partition;
      NodePtr    m_beg_2st_partition;
   };

   template<class Pred>
   static void stable_partition(NodePtr beg, NodePtr end, Pred pred, StablePartitionInfo &info)
   {
      NodePtr bcur = NodeTraits::get_previous(beg);
      NodePtr cur  = beg;
      NodePtr new_f = end;

      std::size_t num1 = 0, num2 = 0;
      while(cur != end){
         if(pred(cur)){
            ++num1;
            bcur = cur;
            cur  = NodeTraits::get_next(cur);
         }
         else{
            ++num2;
            NodePtr last_to_remove = bcur;
            new_f = cur;
            bcur = cur;
            cur  = NodeTraits::get_next(cur);
            BOOST_INTRUSIVE_TRY{
               //Main loop
               while(cur != end){
                  if(pred(cur)){ //Might throw
                     ++num1;
                     //Process current node
                     NodeTraits::set_next    (last_to_remove, cur);
                     NodeTraits::set_previous(cur, last_to_remove);
                     last_to_remove = cur;
                     NodePtr nxt = NodeTraits::get_next(cur);
                     NodeTraits::set_next    (bcur, nxt);
                     NodeTraits::set_previous(nxt, bcur);
                     cur = nxt;
                  }
                  else{
                     ++num2;
                     bcur = cur;
                     cur  = NodeTraits::get_next(cur);
                  }
               }
            }
            BOOST_INTRUSIVE_CATCH(...){
               NodeTraits::set_next    (last_to_remove, new_f);
               NodeTraits::set_previous(new_f, last_to_remove);
               BOOST_INTRUSIVE_RETHROW;
            }
            BOOST_INTRUSIVE_CATCH_END
            NodeTraits::set_next(last_to_remove, new_f);
            NodeTraits::set_previous(new_f, last_to_remove);
            break;
         }
      }
      info.m_num_1st_partition = num1;
      info.m_num_2nd_partition = num2;
      info.m_beg_2st_partition = new_f;
   }

   private:
   static void swap_prev(NodePtr this_node, NodePtr other_node) BOOST_NOEXCEPT
   {
      NodePtr temp(TNodeTraits::get_previous(this_node));
      TNodeTraits::set_previous(this_node, TNodeTraits::get_previous(other_node));
      TNodeTraits::set_previous(other_node, temp);
   }

   static void swap_next(NodePtr this_node, NodePtr other_node) BOOST_NOEXCEPT
   {
      NodePtr temp(TNodeTraits::get_next(this_node));
      TNodeTraits::set_next(this_node, TNodeTraits::get_next(other_node));
      TNodeTraits::set_next(other_node, temp);
   }
};

/// @cond

template<class NodeTraits>
struct get_algo<CircularListAlgorithmsValue, NodeTraits>
{
   typedef CircularListAlgorithms<NodeTraits> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_CIRCULAR_LIST_ALGORITHMS_HPP
