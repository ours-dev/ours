/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Daniel K. O. 2005.
// (C) Copyright Ion Gaztanaga 2007-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <cstddef>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif


namespace boost {
namespace intrusive {

/// @cond

template<class NodeTraits, class F>
struct AvltreeNodeCloner
   //Use public inheritance to avoid MSVC bugs with closures
   :  public detail::EboFunctorHolder<F>
{
   typedef typename NodeTraits::NodePtr  NodePtr;
   typedef detail::EboFunctorHolder<F>  BaseT;

   inline AvltreeNodeCloner(F f)
      :  BaseT(f)
   {}

   NodePtr operator()(NodePtr p)
   {
      NodePtr n = BaseT::get()(p);
      NodeTraits::set_balance(n, NodeTraits::get_balance(p));
      return n;
   }

   NodePtr operator()(NodePtr p) const
   {
      NodePtr n = BaseT::get()(p);
      NodeTraits::set_balance(n, NodeTraits::get_balance(p));
      return n;
   }
};

namespace detail {

template<class TValueTraits, class NodePtrCompare, class ExtraChecker>
struct AvltreeNodeChecker
      : public BstreeNodeChecker<TValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef BstreeNodeChecker<TValueTraits, NodePtrCompare, ExtraChecker> BaseCheckerT;
   typedef TValueTraits                             ValueTraits;
   typedef typename ValueTraits::NodeTraits      NodeTraits;
   typedef typename NodeTraits::ConstNodePtr    ConstNodePtr;

   struct ReturnType
         : public BaseCheckerT::ReturnType
   {
      ReturnType() : m_height(0) {}
      int m_height;
   };

   AvltreeNodeChecker(const NodePtrCompare& comp, ExtraChecker extra_checker)
      : BaseCheckerT(comp, extra_checker)
   {}

   void operator () (ConstNodePtr p,
                     const ReturnType& check_return_left, const ReturnType& check_return_right,
                     ReturnType& check_return)
   {
      const int height_diff = check_return_right.m_height - check_return_left.m_height; (void)height_diff;
      BOOST_INTRUSIVE_INVARIANT_ASSERT(
         (height_diff == -1 && NodeTraits::get_balance(p) == NodeTraits::negative()) ||
         (height_diff ==  0 && NodeTraits::get_balance(p) == NodeTraits::zero()) ||
         (height_diff ==  1 && NodeTraits::get_balance(p) == NodeTraits::positive())
      );
      check_return.m_height = 1 +
         (check_return_left.m_height > check_return_right.m_height ? check_return_left.m_height : check_return_right.m_height);
      BaseCheckerT::operator()(p, check_return_left, check_return_right, check_return);
   }
};

} // namespace detail

/// @endcond

//! avltree_algorithms is configured with a NodeTraits class, which encapsulates the
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
//! <tt>balance</tt>: The type of the balance factor
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
//! <tt>static balance get_balance(const_node_ptr n);</tt>
//!
//! <tt>static void set_balance(node_ptr n, balance b);</tt>
//!
//! <tt>static balance negative();</tt>
//!
//! <tt>static balance zero();</tt>
//!
//! <tt>static balance positive();</tt>
template<class TNodeTraits>
class AvltreeAlgorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeAlgorithms<TNodeTraits>
   #endif
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;
   typedef typename TNodeTraits::Balance         Balance;

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
      //Swap balance
      Balance c = TNodeTraits::get_balance(node1);
      TNodeTraits::set_balance(node1, TNodeTraits::get_balance(node2));
      TNodeTraits::set_balance(node2, c);
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
      TNodeTraits::set_balance(new_node, TNodeTraits::get_balance(node_to_be_replaced));
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::next_node(node_ptr)
   static node_ptr next_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::prev_node(node_ptr)
   static node_ptr prev_node(node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::init(node_ptr)
   static void init(node_ptr n) BOOST_NOEXCEPT;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

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
      BstreeAlgo::init_header(header);
      TNodeTraits::set_balance(header, TNodeTraits::zero());
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
      AvltreeNodeCloner<TNodeTraits, Cloner> new_cloner(cloner);
      BstreeAlgo::clone(source_header, target_header, new_cloner, disposer);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clear_and_dispose(node_ptr,Disposer)
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::find(const_node_ptr,const KeyType&,KeyNodePtrCompare)
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
      (const_node_ptr header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<node_ptr, bool> insert_unique_check
      (const_node_ptr header, node_ptr hint, const KeyType &key
      ,KeyNodePtrCompare comp, insert_commit_data &commit_data);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(node_ptr,node_ptr,const insert_commit_data &)
   static void insert_unique_commit
      (NodePtr header, NodePtr new_value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(header, new_value);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(ConstNodePtr p) BOOST_NOEXCEPT
   {  return TNodeTraits::get_balance(p) == TNodeTraits::zero() && BstreeAlgo::is_header(p);  }


   /// @cond
   static bool verify(NodePtr header)
   {
      std::size_t height;
      std::size_t count;
      return verify_recursion(TNodeTraits::get_parent(header), count, height);
   }

   private:

   static bool verify_recursion(NodePtr n, std::size_t &count, std::size_t &height)
   {
      if (!n){
         count = 0;
         height = 0;
         return true;
      }
      std::size_t leftcount, rightcount;
      std::size_t leftheight, rightheight;
      if(!verify_recursion(TNodeTraits::get_left (n), leftcount,  leftheight) ||
         !verify_recursion(TNodeTraits::get_right(n), rightcount, rightheight) ){
         return false;
      }
      count = 1u + leftcount + rightcount;
      height = 1u + (leftheight > rightheight ? leftheight : rightheight);

      //If equal height, balance must be zero
      if(rightheight == leftheight){
         if(TNodeTraits::get_balance(n) != TNodeTraits::zero()){
            BOOST_ASSERT(0);
            return false;
         }
      }
      //If right is taller than left, then the difference must be at least 1 and the balance positive
      else if(rightheight > leftheight){
         if(rightheight - leftheight > 1 ){
            BOOST_ASSERT(0);
            return false;
         }
         else if(TNodeTraits::get_balance(n) != TNodeTraits::positive()){
            BOOST_ASSERT(0);
            return false;
         }
      }
      //If left is taller than right, then the difference must be at least 1 and the balance negative
      else{
         if(leftheight - rightheight > 1 ){
            BOOST_ASSERT(0);
            return false;
         }
         else if(TNodeTraits::get_balance(n) != TNodeTraits::negative()){
            BOOST_ASSERT(0);
            return false;
         }
      }
      return true;
   }

   static void rebalance_after_erasure
      ( NodePtr header, NodePtr z, const typename BstreeAlgo::data_for_rebalance &info) BOOST_NOEXCEPT
   {
      if(info.y != z){
         TNodeTraits::set_balance(info.y, TNodeTraits::get_balance(z));
      }
      //Rebalance avltree
      rebalance_after_erasure_restore_invariants(header, info.x, info.x_parent);
   }

   static void rebalance_after_erasure_restore_invariants(NodePtr header, NodePtr x, NodePtr x_parent) BOOST_NOEXCEPT
   {
      for ( NodePtr root = TNodeTraits::get_parent(header)
          ; x != root
          ; root = TNodeTraits::get_parent(header), x_parent = TNodeTraits::get_parent(x)) {
         const Balance x_parent_balance = TNodeTraits::get_balance(x_parent);
         //Don't cache x_is_leftchild or similar because x can be null and
         //equal to both x_parent_left and x_parent_right
         const NodePtr x_parent_left (TNodeTraits::get_left(x_parent));
         const NodePtr x_parent_right(TNodeTraits::get_right(x_parent));

         if(x_parent_balance == TNodeTraits::zero()){
            TNodeTraits::set_balance( x_parent, x == x_parent_right ? TNodeTraits::negative() : TNodeTraits::positive() );
            break;       // the height didn't change, let's stop here
         }
         else if(x_parent_balance == TNodeTraits::negative()){
            if (x == x_parent_left) {  ////x is left child or x and sibling are null
               TNodeTraits::set_balance(x_parent, TNodeTraits::zero()); // balanced
               x = x_parent;
            }
            else {
               // x is right child (x_parent_left is the left child)
               BOOST_INTRUSIVE_INVARIANT_ASSERT(x_parent_left);
               if (TNodeTraits::get_balance(x_parent_left) == TNodeTraits::positive()) {
                  // x_parent_left MUST have a right child
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_right(x_parent_left));
                  x = avl_rotate_left_right(x_parent, x_parent_left, header);
               }
               else {
                  avl_rotate_right(x_parent, x_parent_left, header);
                  x = x_parent_left;
               }

               // if changed from negative to NodeTraits::positive(), no need to check above
               if (TNodeTraits::get_balance(x) == TNodeTraits::positive()){
                  break;
               }
            }
         }
         else if(x_parent_balance == TNodeTraits::positive()){
            if (x == x_parent_right) { //x is right child or x and sibling are null
               TNodeTraits::set_balance(x_parent, TNodeTraits::zero()); // balanced
               x = x_parent;
            }
            else {
               // x is left child (x_parent_right is the right child)
               BOOST_INTRUSIVE_INVARIANT_ASSERT(x_parent_right);
               if (TNodeTraits::get_balance(x_parent_right) == TNodeTraits::negative()) {
                  // x_parent_right MUST have then a left child
                  BOOST_INTRUSIVE_INVARIANT_ASSERT(TNodeTraits::get_left(x_parent_right));
                  x = avl_rotate_right_left(x_parent, x_parent_right, header);
               }
               else {
                  avl_rotate_left(x_parent, x_parent_right, header);
                  x = x_parent_right;
               }
               // if changed from NodeTraits::positive() to negative, no need to check above
               if (TNodeTraits::get_balance(x) == TNodeTraits::negative()){
                  break;
               }
            }
         }
         else{
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);  // never reached
         }
      }
   }

   static void rebalance_after_insertion(NodePtr header, NodePtr x) BOOST_NOEXCEPT
   {
      TNodeTraits::set_balance(x, TNodeTraits::zero());
      // Rebalance.
      for(NodePtr root = TNodeTraits::get_parent(header); x != root; root = TNodeTraits::get_parent(header)){
         NodePtr const x_parent(TNodeTraits::get_parent(x));
         NodePtr const x_parent_left(TNodeTraits::get_left(x_parent));
         const Balance x_parent_balance = TNodeTraits::get_balance(x_parent);
         const bool x_is_leftchild(x == x_parent_left);
         if(x_parent_balance == TNodeTraits::zero()){
            // if x is left, parent will have parent->bal_factor = negative
            // else, parent->bal_factor = NodeTraits::positive()
            TNodeTraits::set_balance( x_parent, x_is_leftchild ? TNodeTraits::negative() : TNodeTraits::positive()  );
            x = x_parent;
         }
         else if(x_parent_balance == TNodeTraits::positive()){
            // if x is a left child, parent->bal_factor = zero
            if (x_is_leftchild)
               TNodeTraits::set_balance(x_parent, TNodeTraits::zero());
            else{        // x is a right child, needs rebalancing
               if (TNodeTraits::get_balance(x) == TNodeTraits::negative())
                  avl_rotate_right_left(x_parent, x, header);
               else
                  avl_rotate_left(x_parent, x, header);
            }
            break;
         }
         else if(x_parent_balance == TNodeTraits::negative()){
            // if x is a left child, needs rebalancing
            if (x_is_leftchild) {
               if (TNodeTraits::get_balance(x) == TNodeTraits::positive())
                  avl_rotate_left_right(x_parent, x, header);
               else
                  avl_rotate_right(x_parent, x, header);
            }
            else
               TNodeTraits::set_balance(x_parent, TNodeTraits::zero());
            break;
         }
         else{
            BOOST_INTRUSIVE_INVARIANT_ASSERT(false);  // never reached
         }
      }
   }

   static void left_right_balancing(NodePtr a, NodePtr b, NodePtr c) BOOST_NOEXCEPT
   {
      // balancing...
      const Balance c_balance = TNodeTraits::get_balance(c);
      const Balance zero_balance = TNodeTraits::zero();
      const Balance posi_balance = TNodeTraits::positive();
      const Balance nega_balance = TNodeTraits::negative();
      TNodeTraits::set_balance(c, zero_balance);
      if(c_balance == nega_balance){
         TNodeTraits::set_balance(a, posi_balance);
         TNodeTraits::set_balance(b, zero_balance);
      }
      else if(c_balance == zero_balance){
         TNodeTraits::set_balance(a, zero_balance);
         TNodeTraits::set_balance(b, zero_balance);
      }
      else if(c_balance == posi_balance){
         TNodeTraits::set_balance(a, zero_balance);
         TNodeTraits::set_balance(b, nega_balance);
      }
      else{
         BOOST_INTRUSIVE_INVARIANT_ASSERT(false); // never reached
      }
   }

   static NodePtr avl_rotate_left_right(const NodePtr a, const NodePtr a_oldleft, NodePtr hdr) BOOST_NOEXCEPT
   {  // [note: 'a_oldleft' is 'b']
      //             |                               |         //
      //             a(-2)                           c         //
      //            / \                             / \        //
      //           /   \        ==>                /   \       //
      //      (pos)b    [g]                       b     a      //
      //          / \                            / \   / \     //
      //        [d]  c                         [d]  e f  [g]   //
      //            / \                                        //
      //           e   f                                       //
      const NodePtr c = TNodeTraits::get_right(a_oldleft);
      BstreeAlgo::rotate_left_no_parent_fix(a_oldleft, c);
      //No need to link c with a [NodeTraits::set_parent(c, a) + NodeTraits::set_left(a, c)]
      //as c is not root and another rotation is coming
      BstreeAlgo::rotate_right(a, c, TNodeTraits::get_parent(a), hdr);
      left_right_balancing(a, a_oldleft, c);
      return c;
   }

   static NodePtr avl_rotate_right_left(const NodePtr a, const NodePtr a_oldright, NodePtr hdr) BOOST_NOEXCEPT
   {  // [note: 'a_oldright' is 'b']
      //              |                               |           //
      //              a(pos)                          c           //
      //             / \                             / \          //
      //            /   \                           /   \         //
      //          [d]   b(neg)         ==>         a     b        //
      //               / \                        / \   / \       //
      //              c  [g]                    [d] e  f  [g]     //
      //             / \                                          //
      //            e   f                                         //
      const NodePtr c (TNodeTraits::get_left(a_oldright));
      BstreeAlgo::rotate_right_no_parent_fix(a_oldright, c);
      //No need to link c with a [NodeTraits::set_parent(c, a) + NodeTraits::set_right(a, c)]
      //as c is not root and another rotation is coming.
      BstreeAlgo::rotate_left(a, c, TNodeTraits::get_parent(a), hdr);
      left_right_balancing(a_oldright, a, c);
      return c;
   }

   static void avl_rotate_left(NodePtr x, NodePtr x_oldright, NodePtr hdr) BOOST_NOEXCEPT
   {
      BstreeAlgo::rotate_left(x, x_oldright, TNodeTraits::get_parent(x), hdr);

      // reset the balancing factor
      if (TNodeTraits::get_balance(x_oldright) == TNodeTraits::positive()) {
         TNodeTraits::set_balance(x, TNodeTraits::zero());
         TNodeTraits::set_balance(x_oldright, TNodeTraits::zero());
      }
      else {        // this doesn't happen during insertions
         TNodeTraits::set_balance(x, TNodeTraits::positive());
         TNodeTraits::set_balance(x_oldright, TNodeTraits::negative());
      }
   }

   static void avl_rotate_right(NodePtr x, NodePtr x_oldleft, NodePtr hdr) BOOST_NOEXCEPT
   {
      BstreeAlgo::rotate_right(x, x_oldleft, TNodeTraits::get_parent(x), hdr);

      // reset the balancing factor
      if (TNodeTraits::get_balance(x_oldleft) == TNodeTraits::negative()) {
         TNodeTraits::set_balance(x, TNodeTraits::zero());
         TNodeTraits::set_balance(x_oldleft, TNodeTraits::zero());
      }
      else {        // this doesn't happen during insertions
         TNodeTraits::set_balance(x, TNodeTraits::negative());
         TNodeTraits::set_balance(x_oldleft, TNodeTraits::positive());
      }
   }

   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<AvlTreeAlgorithmsValue, NodeTraits>
{
   typedef AvltreeAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<AvlTreeAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef detail::AvltreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_AVLTREE_ALGORITHMS_HPP
