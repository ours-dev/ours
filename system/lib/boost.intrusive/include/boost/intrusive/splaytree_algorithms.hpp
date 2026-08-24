/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
// The implementation of splay trees is based on the article and code published
// in C++ Users Journal "Implementing Splay Trees in C++" (September 1, 2005).
//
// The splay code has been modified and (supposedly) improved by Ion Gaztanaga.
//
// Here is the copyright notice of the original file containing the splay code:
//
//  splay_tree.h -- implementation of a STL compatible splay tree.
//
//  Copyright (c) 2004 Ralf Mattethat
//
//  Permission to copy, use, modify, sell and distribute this software
//  is granted provided this copyright notice appears in all copies.
//  This software is provided "as is" without express or implied
//  warranty, and with no claim as to its suitability for any purpose.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/detail/uncast.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

#include <cstddef>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond
namespace detail {

template<class NodeTraits>
struct SplaydownAssembleAndFixHeader
{
   typedef typename NodeTraits::NodePtr NodePtr;

   SplaydownAssembleAndFixHeader(NodePtr t, NodePtr header, NodePtr leftmost, NodePtr rightmost) BOOST_NOEXCEPT
      : m_t(t)
      , m_null_node(header)
      , m_l(m_null_node)
      , m_r(m_null_node)
      , m_leftmost(leftmost)
      , m_rightmost(rightmost)
   {}

   ~SplaydownAssembleAndFixHeader()
   {
      this->assemble();

      //Now recover the original header except for the
      //splayed root node.
      //"t_" is the current root and "null_node_" is the header node
      NodeTraits::set_parent(m_null_node, m_t);
      NodeTraits::set_parent(m_t, m_null_node);
      //Recover leftmost/rightmost pointers
      NodeTraits::set_left (m_null_node, m_leftmost);
      NodeTraits::set_right(m_null_node, m_rightmost);
   }

   private:

   void assemble() BOOST_NOEXCEPT
   {
      //procedure assemble;
      //    left(r), right(l) := right(t), left(t);
      //    left(t), right(t) := right(null), left(null);
      //end assemble;
      {  //    left(r), right(l) := right(t), left(t);

         NodePtr const old_t_left  = NodeTraits::get_left(m_t);
         NodePtr const old_t_right = NodeTraits::get_right(m_t);
         NodeTraits::set_right(m_l, old_t_left);
         NodeTraits::set_left (m_r, old_t_right);
         if(old_t_left){
            NodeTraits::set_parent(old_t_left, m_l);
         }
         if(old_t_right){
            NodeTraits::set_parent(old_t_right, m_r);
         }
      }
      {  //    left(t), right(t) := right(null), left(null);
         NodePtr const null_right = NodeTraits::get_right(m_null_node);
         NodePtr const null_left  = NodeTraits::get_left(m_null_node);
         NodeTraits::set_left (m_t, null_right);
         NodeTraits::set_right(m_t, null_left);
         if(null_right){
            NodeTraits::set_parent(null_right, m_t);
         }
         if(null_left){
            NodeTraits::set_parent(null_left, m_t);
         }
      }
   }

   public:
   NodePtr m_t, m_null_node, m_l, m_r, m_leftmost, m_rightmost;
};

}  //namespace detail {
/// @endcond

//!   A splay tree is an implementation of a binary search tree. The tree is
//!   self balancing using the splay algorithm as described in
//!
//!      "Self-Adjusting Binary Search Trees
//!      by Daniel Dominic Sleator and Robert Endre Tarjan
//!      AT&T Bell Laboratories, Murray Hill, NJ
//!      Journal of the ACM, Vol 32, no 3, July 1985, pp 652-686
//!
//! splaytree_algorithms is configured with a NodeTraits class, which encapsulates the
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
class SplaytreeAlgorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeAlgorithms<TNodeTraits>
   #endif
{
   /// @cond
   private:
   typedef BstreeAlgorithms<TNodeTraits> BstreeAlgo;
   /// @endcond

   public:
   typedef typename TNodeTraits::Node            Node;
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename BstreeAlgo::InsertCommitData InsertCommitData;

   public:
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::get_header(const_node_ptr)
   static node_ptr get_header(const_node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::begin_node
   static node_ptr begin_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::end_node
   static node_ptr end_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_tree
   static void swap_tree(node_ptr header1, node_ptr header2);

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr)
   static void swap_nodes(node_ptr node1, node_ptr node2) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr,node_ptr,node_ptr)
   static void swap_nodes(node_ptr node1, node_ptr header1, node_ptr node2, node_ptr header2) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr)
   static void replace_node(node_ptr node_to_be_replaced, node_ptr new_node) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr,node_ptr)
   static void replace_node(node_ptr node_to_be_replaced, node_ptr header, node_ptr new_node) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(node_ptr)
   static void unlink(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink_leftmost_without_rebalance
   static node_ptr unlink_leftmost_without_rebalance(node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::unique(const_node_ptr)
   static bool unique(const_node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::size(const_node_ptr)
   static std::size_t size(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::next_node(node_ptr)
   static node_ptr next_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::prev_node(node_ptr)
   static node_ptr prev_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::init(node_ptr)
   static void init(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::init_header(node_ptr)
   static void init_header(node_ptr header) BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(node_ptr,node_ptr)
   //! Additional notes: the previous node of z is splayed to speed up range deletions.
   static void erase(NodePtr header, NodePtr z) BOOST_NOEXCEPT
   {
      //posibility 1
      if(TNodeTraits::get_left(z)){
         splay_up(BstreeAlgo::prev_node(z), header);
      }

      //possibility 2
      //if(NodeTraits::get_left(z)){
      //   node_ptr l = NodeTraits::get_left(z);
      //   splay_up(l, header);
      //}

      //if(NodeTraits::get_left(z)){
      //   node_ptr l = bstree_algo::prev_node(z);
      //   splay_up_impl(l, z);
      //}

      //possibility 4
      //splay_up(z, header);

      BstreeAlgo::erase(header, z);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_unique
   template<class NodePtrCompare>
   static bool transfer_unique
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      typename BstreeAlgo::insert_commit_data commit_data;
      bool const transferable = BstreeAlgo::insert_unique_check(header1, z, comp, commit_data).second;
      if(transferable){
         erase(header2, z);
         BstreeAlgo::insert_commit(header1, z, commit_data);
         splay_up(z, header1);
      }
      return transferable;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_equal
   template<class NodePtrCompare>
   static void transfer_equal
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      InsertCommitData commit_data;
      splay_down(header1, z, comp);
      BstreeAlgo::insert_equal_upper_bound_check(header1, z, comp, commit_data);
      erase(header2, z);
      BstreeAlgo::insert_commit(header1, z, commit_data);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const_node_ptr,node_ptr,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (const_node_ptr source_header, node_ptr target_header, Cloner cloner, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree_algorithms::clear_and_dispose(node_ptr,Disposer)
   template<class Disposer>
   static void clear_and_dispose(node_ptr header, Disposer disposer) BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: an element with key `key` is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      std::pair<NodePtr, NodePtr> ret = equal_range(header, key, comp);
      std::size_t n = 0;
      while(ret.first != ret.second){
         ++n;
         ret.first = next_node(ret.first);
      }
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::count(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr lower_bound
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      splay_down(detail::uncast(header), key, comp);
      NodePtr y = BstreeAlgo::lower_bound(header, key, comp);
      //splay_up(y, detail::uncast(header));
      return y;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr lower_bound
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::lower_bound(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr upper_bound
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      splay_down(detail::uncast(header), key, comp);
      NodePtr y = BstreeAlgo::upper_bound(header, key, comp);
      //splay_up(y, detail::uncast(header));
      return y;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr upper_bound
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::upper_bound(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const_node_ptr, const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the found node of the lower bound is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr find
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      splay_down(detail::uncast(header), key, comp);
      return BstreeAlgo::find(header, key, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const_node_ptr, const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr find
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::find(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> equal_range
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      splay_down(detail::uncast(header), key, comp);
      std::pair<NodePtr, NodePtr> ret = BstreeAlgo::equal_range(header, key, comp);
      //splay_up(ret.first, detail::uncast(header));
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> equal_range
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::equal_range(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound_range(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> lower_bound_range
      (NodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {
      splay_down(detail::uncast(header), key, comp);
      std::pair<NodePtr, NodePtr> ret = BstreeAlgo::lower_bound_range(header, key, comp);
      //splay_up(ret.first, detail::uncast(header));
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound_range(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> lower_bound_range
      (ConstNodePtr header, const KeyType &key, KeyNodePtrCompare comp)
   {  return BstreeAlgo::lower_bound_range(header, key, comp);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const_node_ptr,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   //! Additional notes: the first node of the range is splayed.
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> bounded_range
      (NodePtr header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed)
   {
      splay_down(detail::uncast(header), lower_key, comp);
      std::pair<NodePtr, NodePtr> ret =
         BstreeAlgo::bounded_range(header, lower_key, upper_key, comp, left_closed, right_closed);
      //splay_up(ret.first, detail::uncast(header));
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const_node_ptr,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   //! Additional note: no splaying is performed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, NodePtr> bounded_range
      (ConstNodePtr header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed)
   {  return BstreeAlgo::bounded_range(header, lower_key, upper_key, comp, left_closed, right_closed);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_upper_bound(node_ptr,node_ptr,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static NodePtr insert_equal_upper_bound
      (NodePtr header, NodePtr new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return BstreeAlgo::insert_equal_upper_bound(header, new_node, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_lower_bound(node_ptr,node_ptr,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static NodePtr insert_equal_lower_bound
      (NodePtr header, NodePtr new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return BstreeAlgo::insert_equal_lower_bound(header, new_node, comp);
   }  

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal(node_ptr,node_ptr,node_ptr,NodePtrCompare)
   //! Additional note: the inserted node is splayed
   template<class NodePtrCompare>
   static NodePtr insert_equal
      (NodePtr header, NodePtr hint, NodePtr new_node, NodePtrCompare comp)
   {
      splay_down(header, new_node, comp);
      return BstreeAlgo::insert_equal(header, hint, new_node, comp);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_before(node_ptr,node_ptr,node_ptr)
   //! Additional note: the inserted node is splayed
   static NodePtr insert_before
      (NodePtr header, NodePtr pos, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_before(header, pos, new_node);
      splay_up(new_node, header);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_back(node_ptr,node_ptr)
   //! Additional note: the inserted node is splayed
   static void push_back(NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::push_back(header, new_node);
      splay_up(new_node, header);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_front(node_ptr,node_ptr)
   //! Additional note: the inserted node is splayed
   static void push_front(NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::push_front(header, new_node);
      splay_up(new_node, header);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   //! Additional note: nodes with the given key are splayed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (NodePtr header, const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data)
   {
      splay_down(header, key, comp);
      return BstreeAlgo::insert_unique_check(header, key, comp, commit_data);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   //! Additional note: nodes with the given key are splayed
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (NodePtr header, NodePtr hint, const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data)
   {
      splay_down(header, key, comp);
      return BstreeAlgo::insert_unique_check(header, hint, key, comp, commit_data);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(node_ptr,node_ptr,const insert_commit_data&)
   static void insert_unique_commit
      (node_ptr header, node_ptr new_value, const insert_commit_data &commit_data) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const_node_ptr p) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::rebalance
   static void rebalance(node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::rebalance_subtree
   static node_ptr rebalance_subtree(node_ptr old_root) BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   // bottom-up splay, use data_ as parent for n    | complexity : logarithmic    | exception : nothrow
   static void splay_up(NodePtr n, NodePtr header) BOOST_NOEXCEPT
   {  priv_splay_up<true>(n, header); }

   // top-down splay | complexity : logarithmic    | exception : strong, note A
   template<class KeyType, class KeyNodePtrCompare>
   static NodePtr splay_down(NodePtr header, const KeyType &key, KeyNodePtrCompare comp, bool *pfound = nullptr)
   {  return priv_splay_down<true>(header, key, comp, pfound);   }

   private:

   /// @cond

   // bottom-up splay, use data_ as parent for n    | complexity : logarithmic    | exception : nothrow
   template<bool SimpleSplay>
   static void priv_splay_up(NodePtr n, NodePtr header) BOOST_NOEXCEPT
   {
      // If (node == header) do a splay for the right most node instead
      // this is to boost performance of equal_range/count on equivalent containers in the case
      // where there are many equal elements at the end
      if(n == header)
         n = TNodeTraits::get_right(header);

      NodePtr t(header);

      if( n == t ) return;

      for( ;; ){
         NodePtr p(TNodeTraits::get_parent(n));
         NodePtr g(TNodeTraits::get_parent(p));

         if( p == t )   break;

         if( g == t ){
            // zig
            rotate(n);
         }
         else if ((TNodeTraits::get_left(p) == n && TNodeTraits::get_left(g) == p)    ||
                  (TNodeTraits::get_right(p) == n && TNodeTraits::get_right(g) == p)  ){
            // zig-zig
            rotate(p);
            rotate(n);
         }
         else {
            // zig-zag
            rotate(n);
            if(!SimpleSplay){
               rotate(n);
            }
         }
      }
   }

   template<bool SimpleSplay, class KeyType, class KeyNodePtrCompare>
   static NodePtr priv_splay_down(NodePtr header, const KeyType &key, KeyNodePtrCompare comp, bool *pfound = nullptr)
   {
      //Most splay tree implementations use a dummy/null node to implement.
      //this function. This has some problems for a generic library like Intrusive:
      //
      // * The node might not have a default constructor.
      // * The default constructor could throw.
      //
      //We already have a header node. Leftmost and rightmost nodes of the tree
      //are not changed when splaying (because the invariants of the tree don't
      //change) We can back up them, use the header as the null node and
      //reassign old values after the function has been completed.
      NodePtr const old_root  = TNodeTraits::get_parent(header);
      NodePtr const leftmost  = TNodeTraits::get_left(header);
      NodePtr const rightmost = TNodeTraits::get_right(header);
      if(leftmost == rightmost){ //Empty or unique node
         if(pfound){
            *pfound = old_root && !comp(key, old_root) && !comp(old_root, key);
         }
         return old_root ? old_root : header;
      }
      else{
         //Initialize "null node" (the header in our case)
         TNodeTraits::set_left (header, NodePtr());
         TNodeTraits::set_right(header, NodePtr());
         //Class that will backup leftmost/rightmost from header, commit the assemble(),
         //and will restore leftmost/rightmost to header even if "comp" throws
         detail::SplaydownAssembleAndFixHeader<TNodeTraits> commit(old_root, header, leftmost, rightmost);
         bool found = false;

         for( ;; ){
            if(comp(key, commit.t_)){
               NodePtr const t_left = TNodeTraits::get_left(commit.t_);
               if(!t_left)
                  break;
               if(comp(key, t_left)){
                  BstreeAlgo::rotate_right_no_parent_fix(commit.t_, t_left);
                  commit.t_ = t_left;
                  if( !TNodeTraits::get_left(commit.t_) )
                     break;
                  link_right(commit.t_, commit.r_);
               }
               else{
                  link_right(commit.t_, commit.r_);
                  if(!SimpleSplay && comp(t_left, key)){
                     if( !TNodeTraits::get_right(commit.t_) )
                        break;
                     link_left(commit.t_, commit.l_);
                  }
               }
            }
            else if(comp(commit.t_, key)){
               NodePtr const t_right = TNodeTraits::get_right(commit.t_);
               if(!t_right)
                  break;

               if(comp(t_right, key)){
                     BstreeAlgo::rotate_left_no_parent_fix(commit.t_, t_right);
                     commit.t_ = t_right;
                     if( !TNodeTraits::get_right(commit.t_) )
                        break;
                     link_left(commit.t_, commit.l_);
               }
               else{
                  link_left(commit.t_, commit.l_);
                  if(!SimpleSplay && comp(key, t_right)){
                     if( !TNodeTraits::get_left(commit.t_) )
                        break;
                     link_right(commit.t_, commit.r_);
                  }
               }
            }
            else{
               found = true;
               break;
            }
         }

         //commit.~splaydown_assemble_and_fix_header<NodeTraits>() will first
         //"assemble()" + link the new root & recover header's leftmost & rightmost
         if(pfound){
            *pfound = found;
         }
         return commit.t_;
      }
   }

   // break link to left child node and attach it to left tree pointed to by l   | complexity : constant | exception : nothrow
   static void link_left(NodePtr & t, NodePtr & l) BOOST_NOEXCEPT
   {
      //procedure link_left;
      //    t, l, right(l) := right(t), t, t
      //end link_left
      TNodeTraits::set_right(l, t);
      TNodeTraits::set_parent(t, l);
      l = t;
      t = TNodeTraits::get_right(t);
   }

   // break link to right child node and attach it to right tree pointed to by r | complexity : constant | exception : nothrow
   static void link_right(NodePtr & t, NodePtr & r) BOOST_NOEXCEPT
   {
      //procedure link_right;
      //    t, r, left(r) := left(t), t, t
      //end link_right;
      TNodeTraits::set_left(r, t);
      TNodeTraits::set_parent(t, r);
      r = t;
      t = TNodeTraits::get_left(t);
   }

   // rotate n with its parent                     | complexity : constant    | exception : nothrow
   static void rotate(NodePtr n) BOOST_NOEXCEPT
   {
      //procedure rotate_left;
      //    t, right(t), left(right(t)) := right(t), left(right(t)), t
      //end rotate_left;
      NodePtr p = TNodeTraits::get_parent(n);
      NodePtr g = TNodeTraits::get_parent(p);
      //Test if g is header before breaking tree
      //invariants that would make is_header invalid
      bool g_is_header = BstreeAlgo::is_header(g);

      if(TNodeTraits::get_left(p) == n){
         TNodeTraits::set_left(p, TNodeTraits::get_right(n));
         if(TNodeTraits::get_left(p))
            TNodeTraits::set_parent(TNodeTraits::get_left(p), p);
         TNodeTraits::set_right(n, p);
      }
      else{ // must be ( p->right == n )
         TNodeTraits::set_right(p, TNodeTraits::get_left(n));
         if(TNodeTraits::get_right(p))
            TNodeTraits::set_parent(TNodeTraits::get_right(p), p);
         TNodeTraits::set_left(n, p);
      }

      TNodeTraits::set_parent(p, n);
      TNodeTraits::set_parent(n, g);

      if(g_is_header){
         if(TNodeTraits::get_parent(g) == p)
            TNodeTraits::set_parent(g, n);
         else{//must be ( g->right == p )
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);
            TNodeTraits::set_right(g, n);
         }
      }
      else{
         if(TNodeTraits::get_left(g) == p)
            TNodeTraits::set_left(g, n);
         else  //must be ( g->right == p )
            TNodeTraits::set_right(g, n);
      }
   }

   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<SplayTreeAlgorithmsValue, NodeTraits>
{
   typedef SplaytreeAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<SplayTreeAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef detail::BstreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SPLAYTREE_ALGORITHMS_HPP
