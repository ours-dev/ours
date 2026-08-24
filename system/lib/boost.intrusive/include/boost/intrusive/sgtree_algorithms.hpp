/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
//
// Scapegoat tree algorithms are taken from the paper titled:
// "Scapegoat Trees" by Igal Galperin Ronald L. Rivest.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_SGTREE_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_SGTREE_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <cstddef>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//! sgtree_algorithms is configured with a NodeTraits class, which encapsulates the
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
class SgtreeAlgorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeAlgorithms<TNodeTraits>
   #endif
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::NodePtr       NodePtr;
   typedef typename TNodeTraits::ConstNodePtr ConstNodePtr;

   /// @cond
   private:

   typedef BstreeAlgorithms<TNodeTraits>  BstreeAlgo;

   /// @endcond

   public:
   //! This type is the information that will be
   //! filled by insert_unique_check
   struct InsertCommitData
      : BstreeAlgo::InsertCommitData
   {
      std::size_t m_depth;
   };

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::get_header(const_node_ptr)
   static node_ptr get_header(const_node_ptr n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::begin_node
   static node_ptr begin_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::end_node
   static node_ptr end_node(const_node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_tree
   static void swap_tree(node_ptr header1, node_ptr header2) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr)
   static void swap_nodes(node_ptr node1, node_ptr node2) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::swap_nodes(node_ptr,node_ptr,node_ptr,node_ptr)
   static void swap_nodes(node_ptr node1, node_ptr header1, node_ptr node2, node_ptr header2) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr)
   static void replace_node(node_ptr node_to_be_replaced, node_ptr new_node) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::replace_node(node_ptr,node_ptr,node_ptr)
   static void replace_node(node_ptr node_to_be_replaced, node_ptr header, node_ptr new_node) BOOST_NOEXCEPT;

   //Unlink is not possible since tree metadata is needed to update the tree
   //!static void unlink(node_ptr n) BOOST_NOEXCEPT;

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
   template<class AlphaByMaxSize>
   static NodePtr erase(NodePtr header, NodePtr z, std::size_t tree_size, std::size_t &max_tree_size, AlphaByMaxSize alpha_by_maxsize)
   {
      BstreeAlgo::erase(header, z);
      --tree_size;
      if (tree_size > 0 &&
          tree_size < static_cast<std::size_t>(alpha_by_maxsize(max_tree_size))){
         BstreeAlgo::rebalance(header);
         max_tree_size = tree_size;
      }
      return z;
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::clone(const_node_ptr,node_ptr,Cloner,Disposer)
   template <class Cloner, class Disposer>
   static void clone
      (const_node_ptr source_header, node_ptr target_header, Cloner cloner, Disposer disposer);

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
      (const_node_ptr header, const KeyType &lower_key, const KeyType &upper_key, KeyNodePtrCompare comp
      , bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree_algorithms::count(const_node_ptr,const KeyType&,KeyNodePtrCompare)
   template<class KeyType, class KeyNodePtrCompare>
   static std::size_t count(const_node_ptr header, const KeyType &key, KeyNodePtrCompare comp);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_upper_bound(node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare, class HAlpha>
   static NodePtr insert_equal_upper_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size)
   {
      std::size_t depth;
      BstreeAlgo::insert_equal_upper_bound(h, new_node, comp, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal_lower_bound(node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare, class HAlpha>
   static NodePtr insert_equal_lower_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size)
   {
      std::size_t depth;
      BstreeAlgo::insert_equal_lower_bound(h, new_node, comp, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_equal(node_ptr,node_ptr,node_ptr,NodePtrCompare)
   template<class NodePtrCompare, class HAlpha>
   static NodePtr insert_equal
      (NodePtr header, NodePtr hint, NodePtr new_node, NodePtrCompare comp
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size)
   {
      std::size_t depth;
      BstreeAlgo::insert_equal(header, hint, new_node, comp, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_before(node_ptr,node_ptr,node_ptr)
   template<class HAlpha>
   static NodePtr insert_before
      (NodePtr header, NodePtr pos, NodePtr new_node
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size)
   {
      std::size_t depth;
      BstreeAlgo::insert_before(header, pos, new_node, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
      return new_node;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_back(node_ptr,node_ptr)
   template<class HAlpha>
   static void push_back(NodePtr header, NodePtr new_node
         ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size) BOOST_NOEXCEPT
   {
      std::size_t depth;
      BstreeAlgo::push_back(header, new_node, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::push_front(node_ptr,node_ptr)
   template<class HAlpha>
   static void push_front(NodePtr header, NodePtr new_node
         ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size) BOOST_NOEXCEPT
   {
      std::size_t depth;
      BstreeAlgo::push_front(header, new_node, &depth);
      rebalance_after_insertion(new_node, depth, tree_size+1, h_alpha, max_tree_size);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (ConstNodePtr header,  const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data)
   {
      std::size_t depth;
      std::pair<NodePtr, bool> ret =
         BstreeAlgo::insert_unique_check(header, key, comp, commit_data, &depth);
      commit_data.m_depth = depth;
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_check(const_node_ptr,node_ptr,const KeyType&,KeyNodePtrCompare,insert_commit_data&)
   template<class KeyType, class KeyNodePtrCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      (ConstNodePtr header, NodePtr hint, const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data)
   {
      std::size_t depth;
      std::pair<NodePtr, bool> ret =
         BstreeAlgo::insert_unique_check
            (header, hint, key, comp, commit_data, &depth);
      commit_data.m_depth = depth;
      return ret;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::insert_unique_commit(node_ptr,node_ptr,const insert_commit_data&)
   template<class HAlpha>
   inline static void insert_unique_commit
      (NodePtr header, NodePtr new_value, const InsertCommitData &commit_data
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size)
   {  return insert_commit(header, new_value, commit_data, tree_size, h_alpha, max_tree_size);  }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_unique
   template<class NodePtrCompare, class HAlpha, class AlphaByMaxSize>
   static bool transfer_unique
      ( NodePtr header1, NodePtrCompare comp, std::size_t tree1_size, std::size_t &max_tree1_size
      , NodePtr header2, NodePtr z,   std::size_t tree2_size, std::size_t &max_tree2_size
      ,HAlpha h_alpha, AlphaByMaxSize alpha_by_maxsize)
   {
      InsertCommitData commit_data;
      bool const transferable = insert_unique_check(header1, z, comp, commit_data).second;
      if(transferable){
         erase(header2, z, tree2_size, max_tree2_size, alpha_by_maxsize);
         insert_commit(header1, z, commit_data, tree1_size, h_alpha, max_tree1_size);
      }
      return transferable;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_equal
   template<class NodePtrCompare, class HAlpha, class AlphaByMaxSize>
   static void transfer_equal
      ( NodePtr header1, NodePtrCompare comp, std::size_t tree1_size, std::size_t &max_tree1_size
      , NodePtr header2, NodePtr z,   std::size_t tree2_size, std::size_t &max_tree2_size
      ,HAlpha h_alpha, AlphaByMaxSize alpha_by_maxsize)
   {
      InsertCommitData commit_data;
      insert_equal_upper_bound_check(header1, z, comp, commit_data);
      erase(header2, z, tree2_size, max_tree2_size, alpha_by_maxsize);
      insert_commit(header1, z, commit_data, tree1_size, h_alpha, max_tree1_size);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const_node_ptr p) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static void rebalance(node_ptr header) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree_algorithms::rebalance_subtree
   static node_ptr rebalance_subtree(node_ptr old_root) BOOST_NOEXCEPT
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   /// @cond
   private:

   template<class KeyType, class KeyNodePtrCompare>
   static void insert_equal_upper_bound_check
      (NodePtr header,  const KeyType &key
      ,KeyNodePtrCompare comp, InsertCommitData &commit_data)
   {
      std::size_t depth;
      BstreeAlgo::insert_equal_upper_bound_check(header, key, comp, commit_data, &depth);
      commit_data.m_depth = depth;
   }

   template<class HAlpha>
   static void insert_commit
      (NodePtr header, NodePtr new_value, const InsertCommitData &commit_data
      ,std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_unique_commit(header, new_value, commit_data);
      rebalance_after_insertion(new_value, commit_data.m_depth, tree_size+1, h_alpha, max_tree_size);
   }

   template<class HAlpha>
   static void rebalance_after_insertion
      (NodePtr x, std::size_t depth
      , std::size_t tree_size, HAlpha h_alpha, std::size_t &max_tree_size) BOOST_NOEXCEPT
   {
      if(tree_size > max_tree_size)
         max_tree_size = tree_size;

      if(tree_size > 2 && //Nothing to do with only the root
         //Check if the root node is unbalanced
         //Scapegoat paper depth counts root depth as zero and "depth" counts root as 1,
         //but since "depth" is the depth of the ancestor of x, i == depth
         depth > h_alpha(tree_size)){

         //Find the first non height-balanced node
         //as described in the section 4.2 of the paper.
         //This method is the alternative method described
         //in the paper. Authors claim that this method
         //may tend to yield more balanced trees on the average
         //than the weight balanced method.
         NodePtr s = x;
         std::size_t size = 1;
         for(std::size_t ancestor = 1; ancestor != depth; ++ancestor){
            const NodePtr s_parent = TNodeTraits::get_parent(s);
            const NodePtr s_parent_left = TNodeTraits::get_left(s_parent);
            //Obtain parent's size (previous size + parent + sibling tree)
            const NodePtr s_sibling = s_parent_left == s ? TNodeTraits::get_right(s_parent) : s_parent_left;
            size += 1 + BstreeAlgo::subtree_size(s_sibling);
            s = s_parent;
            if(ancestor > h_alpha(size)){ //is 's' scapegoat?
               BstreeAlgo::rebalance_subtree(s);
               return;
            }
         }
         //The whole tree must be rebuilt
         max_tree_size = tree_size;
         BstreeAlgo::rebalance_subtree(TNodeTraits::get_parent(s));
      }
   }
   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<SgTreeAlgorithmsValue, NodeTraits>
{
   typedef SgtreeAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<SgTreeAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef detail::BstreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SGTREE_ALGORITHMS_HPP
