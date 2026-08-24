/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2014.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
//
// The tree destruction algorithm is based on Julienne Walker and The EC Team code:
//
// This code is in the public domain. Anyone may use it or change it in any way that
// they see fit. The author assumes no responsibility for damages incurred through
// use of the original code or any variations thereof.
//
// It is requested, but not required, that due credit is given to the original author
// and anyone who has modified the code through a header comment, such as this one.

#ifndef BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <cstddef>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

template<class NodeTraits, class F>
struct RbtreeNodeCloner
   //Use public inheritance to avoid MSVC bugs with closures
   :  public detail::EboFunctorHolder<F>
{
   typedef typename NodeTraits::NodePtr  NodePtr;
   typedef detail::EboFunctorHolder<F>  BaseT;

   explicit RbtreeNodeCloner(F f)
      :  BaseT(f)
   {}

   NodePtr operator()(NodePtr p)
   {
      NodePtr n = BaseT::get()(p);
      NodeTraits::set_color(n, NodeTraits::get_color(p));
      return n;
   }
};

namespace detail {

template<class TValueTraits, class NodePtrCompare, class ExtraChecker>
struct RbtreeNodeChecker
   : public BstreeNodeChecker<TValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef BstreeNodeChecker<TValueTraits, NodePtrCompare, ExtraChecker> BaseCheckerT;
   typedef TValueTraits                             ValueTraits;
   typedef typename ValueTraits::NodeTraits      NodeTraits;
   typedef typename NodeTraits::ConstNodePtr    ConstNodePtr;
   typedef typename NodeTraits::NodePtr          NodePtr;

   struct ReturnType
         : public BaseCheckerT::ReturnType
   {
      ReturnType() : m_black_count(0) {}
      std::size_t m_black_count;
   };

   RbtreeNodeChecker(const NodePtrCompare& comp, ExtraChecker extra_checker)
      : BaseCheckerT(comp, extra_checker)
   {}

   void operator () (ConstNodePtr p,
                     const ReturnType& check_return_left, const ReturnType& check_return_right,
                     ReturnType& check_return)
   {

      if (NodeTraits::get_color(p) == NodeTraits::red()){
         //Red nodes have black children
         const NodePtr p_left(NodeTraits::get_left(p));   (void)p_left;
         const NodePtr p_right(NodeTraits::get_right(p)); (void)p_right;
         BOOST_INTRUSIVE_INVARIANT_ASSERT(!p_left  || NodeTraits::get_color(p_left)  == NodeTraits::black());
         BOOST_INTRUSIVE_INVARIANT_ASSERT(!p_right || NodeTraits::get_color(p_right) == NodeTraits::black());
         //Red node can't be root
         BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_parent(NodeTraits::get_parent(p)) != p);
      }
      //Every path to p contains the same number of black nodes
      const std::size_t l_black_count = check_return_left.m_black_count;
      BOOST_INTRUSIVE_INVARIANT_ASSERT(l_black_count == check_return_right.m_black_count);
      check_return.m_black_count = l_black_count +
         static_cast<std::size_t>(NodeTraits::get_color(p) == NodeTraits::black());
      BaseCheckerT::operator()(p, check_return_left, check_return_right, check_return);
   }
};

} // namespace detail

#endif   //#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

//! rbtree_algorithms provides basic algorithms to manipulate
//! nodes forming a red-black tree. The insertion and deletion algorithms are
//! based on those in Cormen, Leiserson, and Rivest, Introduction to Algorithms
//! (MIT Press, 1990), except that
//!
//! (1) the header node is maintained with links not only to the root
//! but also to the leftmost node of the tree, to enable constant time
//! begin(), and to the rightmost node of the tree, to enable linear time
//! performance when used with the generic set algorithms (set_union,
//! etc.);
//!
//! (2) when a node being deleted has two children its successor node is
//! relinked into its place, rather than copied, so that the only
//! pointers invalidated are those referring to the deleted node.
//!
//! rbtree_algorithms is configured with a NodeTraits class, which encapsulates the
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
//! <tt>color</tt>: The type that can store the color of a node
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
//!
//! <tt>static color get_color(const_node_ptr n);</tt>
//!
//! <tt>static void set_color(node_ptr n, color c);</tt>
//!
//! <tt>static color black();</tt>
//!
//! <tt>static color red();</tt>
template<class TNodeTraits>
class RbtreeAlgorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeAlgorithms<TNodeTraits>
   #endif
{
   public:
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::Node            Node;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;
   typedef typename TNodeTraits::Color           Color;

   /// @cond
   private:

   typedef BstreeAlgorithms<TNodeTraits>  BstreeAlgo;

   /// @endcond

   public:

   //! This type is the information that will be
   //! filled by insert_unique_check
   typedef typename BstreeAlgo::InsertCommitData InsertCommitData;

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::get_header(const_node_ptr)
   static node_ptr get_header(const_node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::begin_node
   static node_ptr begin_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::end_node
   static node_ptr end_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_tree
   static void swap_tree(node_ptr header1, node_ptr header2) BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr)
   static void swap_nodes(NodePtr node1, NodePtr node2) BOOST_NOEXCEPT
   {
      if(node1 == node2)
         return;

      NodePtr header1(BstreeAlgo::get_header(node1)), header2(BstreeAlgo::get_header(node2));
      swap_nodes(node1, header1, node2, header2);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr,node_ptr,node_ptr)
   static void swap_nodes(NodePtr node1, NodePtr header1, NodePtr node2, NodePtr header2) BOOST_NOEXCEPT
   {
      if(node1 == node2)   return;

      BstreeAlgo::swap_nodes(node1, header1, node2, header2);
      //Swap color
      Color c = TNodeTraits::get_color(node1);
      TNodeTraits::set_color(node1, TNodeTraits::get_color(node2));
      TNodeTraits::set_color(node2, c);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr)
   static void replace_node(NodePtr node_to_be_replaced, NodePtr new_node) BOOST_NOEXCEPT
   {
      if(node_to_be_replaced == new_node)
         return;
      replace_node(node_to_be_replaced, BstreeAlgo::get_header(node_to_be_replaced), new_node);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr,node_ptr)
   static void replace_node(NodePtr node_to_be_replaced, NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::replace_node(node_to_be_replaced, header, new_node);
      TNodeTraits::set_color(new_node, TNodeTraits::get_color(node_to_be_replaced));
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(node_ptr)
   static void unlink(NodePtr n) BOOST_NOEXCEPT
   {
      NodePtr x = TNodeTraits::get_parent(n);
      if(x){
         while(!is_header(x))
            x = TNodeTraits::get_parent(x);
         erase(x, n);
      }
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink_leftmost_without_rebalance
   static node_ptr unlink_leftmost_without_rebalance(node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::unique(const_node_ptr)
   static bool unique(const_node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::size(const_node_ptr)
   static std::size_t size(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::next_node(const_node_ptr)
   static node_ptr next_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::prev_node(const_node_ptr)
   static node_ptr prev_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::init(node_ptr)
   static void init(node_ptr n) BOOST_NOEXCEPT;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::init_header(node_ptr)
   static void init_header(NodePtr header) BOOST_NOEXCEPT
   {
      BstreeAlgo::init_header(header);
      TNodeTraits::set_color(header, TNodeTraits::red());
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(node_ptr,node_ptr)
   static NodePtr erase(NodePtr header, NodePtr z) BOOST_NOEXCEPT
   {
      typename BstreeAlgo::data_for_rebalance info;
      BstreeAlgo::erase(header, z, info);
      rebalance_after_erasure(header, z, info);
      return z;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_unique
   template<class NodePtrCompare>
   static bool transfer_unique
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      typename BstreeAlgo::data_for_rebalance info;
      bool const transferred = BstreeAlgo::transfer_unique(header1, comp, header2, z, info);
      if(transferred){
         rebalance_after_erasure(header2, z, info);
         rebalance_after_insertion(header1, z);
      }
      return transferred;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_equal
   template<class NodePtrCompare>
   static void transfer_equal
      (NodePtr header1, NodePtrCompare comp, NodePtr header2, NodePtr z)
   {
      typename BstreeAlgo::data_for_rebalance info;
      BstreeAlgo::transfer_equal(header1, comp, header2, z, info);
      rebalance_after_erasure(header2, z, info);
      rebalance_after_insertion(header1, z);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const_node_ptr,node_ptr,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (ConstNodePtr source_header, NodePtr target_header, Cloner cloner, Disposer disposer)
   {
      RbtreeNodeCloner<TNodeTraits, Cloner> new_cloner(cloner);
      BstreeAlgo::clone(source_header, target_header, new_cloner, disposer);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clear_and_dispose(const_node_ptr,Disposer)
   template<class Disposer>
   static void clear_and_dispose(node_ptr header, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::lower_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr lower_bound
      (const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::upper_bound(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr upper_bound
      (const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const_node_ptr, const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static node_ptr find
      (const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::equal_range(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> equal_range
      (const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   //! @copydoc ::boost::intrusive::bstree_algorithms::bounded_range(const_node_ptr,const KeyType&,const KeyType&,KeyNodePtrCompare,bool,bool)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, node_ptr> bounded_range
      (const_node_ptr eader, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count(const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_upper_bound(node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare>
   static NodePtr insert_equal_upper_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp)
   {
      BstreeAlgo::insert_equal_upper_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_lower_bound(node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare>
   static NodePtr insert_equal_lower_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp)
   {
      BstreeAlgo::insert_equal_lower_bound(h, new_node, comp);
      rebalance_after_insertion(h, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal(node_ptr,node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare>
   static NodePtr insert_equal
      (NodePtr header, NodePtr hint, NodePtr new_node, NodePtrCompare comp)
   {
      BstreeAlgo::insert_equal(header, hint, new_node, comp);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_before(node_ptr,node_ptr,node_ptr)
   static NodePtr insert_before
      (NodePtr header, NodePtr pos, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_before(header, pos, new_node);
      rebalance_after_insertion(header, new_node);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_back(node_ptr,node_ptr)
   static void push_back(NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::push_back(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_front(node_ptr,node_ptr)
   static void push_front(NodePtr header, NodePtr new_node) BOOST_NOEXCEPT
   {
      BstreeAlgo::push_front(header, new_node);
      rebalance_after_insertion(header, new_node);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const_node_ptr header,  const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,const_node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const_node_ptr header, node_ptr hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(node_ptr,node_ptr,const insert_commit_data&)
   static void insert_unique_commit
      (NodePtr header, NodePtr new_value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(header, new_value);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(ConstNodePtr p) BOOST_NOEXCEPT
   {
      return TNodeTraits::get_color(p) == TNodeTraits::red() &&
            BstreeAlgo::is_header(p);
   }

   /// @cond
   private:

   static void rebalance_after_erasure
      ( NodePtr header, NodePtr z, const typename BstreeAlgo::data_for_rebalance &info) BOOST_NOEXCEPT
   {
      Color new_z_color;
      if(info.y != z){
         new_z_color = TNodeTraits::get_color(info.y);
         TNodeTraits::set_color(info.y, TNodeTraits::get_color(z));
      }
      else{
         new_z_color = TNodeTraits::get_color(z);
      }
      //Rebalance rbtree if needed
      if(new_z_color != TNodeTraits::red()){
         rebalance_after_erasure_restore_invariants(header, info.x, info.x_parent);
      }
   }

   static void rebalance_after_erasure_restore_invariants(NodePtr header, NodePtr x, NodePtr x_parent) BOOST_NOEXCEPT
   {
      while(1){
         if(x_parent == header || (x && TNodeTraits::get_color(x) != TNodeTraits::black())){
            break;
         }
         //Don't cache x_is_leftchild or similar because x can be null and
         //equal to both x_parent_left and x_parent_right
         const NodePtr x_parent_left(TNodeTraits::get_left(x_parent));
         if(x == x_parent_left){ //x is left child
            NodePtr w = TNodeTraits::get_right(x_parent);
            BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
            if(TNodeTraits::get_color(w) == TNodeTraits::red()){
               TNodeTraits::set_color(w, TNodeTraits::black());
               TNodeTraits::set_color(x_parent, TNodeTraits::red());
               BstreeAlgo::rotate_left(x_parent, w, TNodeTraits::get_parent(x_parent), header);
               w = TNodeTraits::get_right(x_parent);
               BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
            }
            NodePtr const w_left (TNodeTraits::get_left(w));
            NodePtr const w_right(TNodeTraits::get_right(w));
            if((!w_left  || TNodeTraits::get_color(w_left)  == TNodeTraits::black()) &&
               (!w_right || TNodeTraits::get_color(w_right) == TNodeTraits::black())){
               TNodeTraits::set_color(w, TNodeTraits::red());
               x = x_parent;
               x_parent = TNodeTraits::get_parent(x_parent);
            }
            else {
               if(!w_right || TNodeTraits::get_color(w_right) == TNodeTraits::black()){
                  TNodeTraits::set_color(w_left, TNodeTraits::black());
                  TNodeTraits::set_color(w, TNodeTraits::red());
                  BstreeAlgo::rotate_right(w, w_left, TNodeTraits::get_parent(w), header);
                  w = TNodeTraits::get_right(x_parent);
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
               }
               TNodeTraits::set_color(w, TNodeTraits::get_color(x_parent));
               TNodeTraits::set_color(x_parent, TNodeTraits::black());
               const NodePtr new_wright(TNodeTraits::get_right(w));
               if(new_wright)
                  TNodeTraits::set_color(new_wright, TNodeTraits::black());
               BstreeAlgo::rotate_left(x_parent, TNodeTraits::get_right(x_parent), TNodeTraits::get_parent(x_parent), header);
               break;
            }
         }
         else {
            // same as above, with right_ <-> left_.
            NodePtr w = x_parent_left;
            BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
            if(TNodeTraits::get_color(w) == TNodeTraits::red()){
               TNodeTraits::set_color(w, TNodeTraits::black());
               TNodeTraits::set_color(x_parent, TNodeTraits::red());
               BstreeAlgo::rotate_right(x_parent, w, TNodeTraits::get_parent(x_parent), header);
               w = TNodeTraits::get_left(x_parent);
               BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
            }
            NodePtr const w_left (TNodeTraits::get_left(w));
            NodePtr const w_right(TNodeTraits::get_right(w));
            if((!w_right || TNodeTraits::get_color(w_right) == TNodeTraits::black()) &&
               (!w_left  || TNodeTraits::get_color(w_left)  == TNodeTraits::black())){
               TNodeTraits::set_color(w, TNodeTraits::red());
               x = x_parent;
               x_parent = TNodeTraits::get_parent(x_parent);
            }
            else {
               if(!w_left || TNodeTraits::get_color(w_left) == TNodeTraits::black()){
                  TNodeTraits::set_color(w_right, TNodeTraits::black());
                  TNodeTraits::set_color(w, TNodeTraits::red());
                  BstreeAlgo::rotate_left(w, w_right, TNodeTraits::get_parent(w), header);
                  w = TNodeTraits::get_left(x_parent);
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(w);
               }
               TNodeTraits::set_color(w, TNodeTraits::get_color(x_parent));
               TNodeTraits::set_color(x_parent, TNodeTraits::black());
               const NodePtr new_wleft(TNodeTraits::get_left(w));
               if(new_wleft)
                  TNodeTraits::set_color(new_wleft, TNodeTraits::black());
               BstreeAlgo::rotate_right(x_parent, TNodeTraits::get_left(x_parent), TNodeTraits::get_parent(x_parent), header);
               break;
            }
         }
      }
      if(x)
         TNodeTraits::set_color(x, TNodeTraits::black());
   }

   static void rebalance_after_insertion(NodePtr header, NodePtr p) BOOST_NOEXCEPT
   {
      TNodeTraits::set_color(p, TNodeTraits::red());
      while(1){
         NodePtr p_parent(TNodeTraits::get_parent(p));
         const NodePtr p_grandparent(TNodeTraits::get_parent(p_parent));
         if(p_parent == header || TNodeTraits::get_color(p_parent) == TNodeTraits::black() || p_grandparent == header){
            break;
         }

         TNodeTraits::set_color(p_grandparent, TNodeTraits::red());
         NodePtr const p_grandparent_left (TNodeTraits::get_left (p_grandparent));
         bool const p_parent_is_left_child = p_parent == p_grandparent_left;
         NodePtr const x(p_parent_is_left_child ? TNodeTraits::get_right(p_grandparent) : p_grandparent_left);

         if(x && TNodeTraits::get_color(x) == TNodeTraits::red()){
            TNodeTraits::set_color(x, TNodeTraits::black());
            TNodeTraits::set_color(p_parent, TNodeTraits::black());
            p = p_grandparent;
         }
         else{ //Final step
            const bool p_is_left_child(TNodeTraits::get_left(p_parent) == p);
            if(p_parent_is_left_child){ //p_parent is left child
               if(!p_is_left_child){ //p is right child
                  BstreeAlgo::rotate_left_no_parent_fix(p_parent, p);
                  //No need to link p and p_grandparent:
                  //    [NodeTraits::set_parent(p, p_grandparent) + NodeTraits::set_left(p_grandparent, p)]
                  //as p_grandparent is not the header, another rotation is coming and p_parent
                  //will be the left child of p_grandparent
                  p_parent = p;
               }
               BstreeAlgo::rotate_right(p_grandparent, p_parent, TNodeTraits::get_parent(p_grandparent), header);
            }
            else{  //p_parent is right child
               if(p_is_left_child){ //p is left child
                  BstreeAlgo::rotate_right_no_parent_fix(p_parent, p);
                  //No need to link p and p_grandparent:
                  //    [NodeTraits::set_parent(p, p_grandparent) + NodeTraits::set_right(p_grandparent, p)]
                  //as p_grandparent is not the header, another rotation is coming and p_parent
                  //will be the right child of p_grandparent
                  p_parent = p;
               }
               BstreeAlgo::rotate_left(p_grandparent, p_parent, TNodeTraits::get_parent(p_grandparent), header);
            }
            TNodeTraits::set_color(p_parent, TNodeTraits::black());
            break;
         }
      }
      TNodeTraits::set_color(TNodeTraits::get_parent(header), TNodeTraits::black());
   }
   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<RbTreeAlgorithmsValue, NodeTraits>
{
   typedef RbtreeAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<RbTreeAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
    typedef detail::RbtreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_RBTREE_ALGORITHMS_HPP
