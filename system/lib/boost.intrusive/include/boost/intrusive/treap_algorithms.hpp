/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2014.
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_TREAP_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_TREAP_ALGORITHMS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <cstddef>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/bstree_algorithms.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

namespace detail
{

template<class TValueTraits, class NodePtrPrioCompare, class ExtraChecker>
struct TreapNodeExtraChecker
      : public ExtraChecker
{
   typedef ExtraChecker                            BaseCheckerT;
   typedef TValueTraits                             ValueTraits;
   typedef typename ValueTraits::NodeTraits      NodeTraits;
   typedef typename NodeTraits::ConstNodePtr ConstNodePtr;

   typedef typename BaseCheckerT::ReturnType    ReturnType;

   TreapNodeExtraChecker(const NodePtrPrioCompare& prio_comp, ExtraChecker extra_checker)
      : BaseCheckerT(extra_checker), m_prio_comp(prio_comp)
   {}

   void operator () (ConstNodePtr p,
                     const ReturnType& check_return_left, const ReturnType& check_return_right,
                     ReturnType& check_return)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeTraits::get_left(p) || !m_prio_comp(NodeTraits::get_left(p), p));
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeTraits::get_right(p) || !m_prio_comp(NodeTraits::get_right(p), p));
      BaseCheckerT::operator()(p, check_return_left, check_return_right, check_return);
   }

   const NodePtrPrioCompare m_prio_comp;
};

} // namespace detail

#endif   //#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

//! treap_algorithms provides basic algorithms to manipulate
//! nodes forming a treap.
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
//! treap_algorithms is configured with a NodeTraits class, which encapsulates the
//! information about the node to be manipulated. NodeTraits must support the
//! following interface:
//!
//! <b>Typedefs</b>:
//!
//! <tt>node</tt>: The type of the node that forms the treap
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
class TreapAlgorithms
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeAlgorithms<TNodeTraits>
   #endif
{
   public:
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::Node            Node;
   typedef typename TNodeTraits::NodePtr       NodePtr;
   typedef typename TNodeTraits::ConstNodePtr ConstNodePtr;

   /// @cond
   private:

   typedef BstreeAlgorithms<TNodeTraits>  BstreeAlgo;

   class RerotateOnDestroy
   {
      RerotateOnDestroy& operator=(const RerotateOnDestroy&);

      public:
      RerotateOnDestroy(NodePtr header, NodePtr p, std::size_t &n)
         :  m_header(header), m_p(p), m_n(n), m_remove_it(true)
      {}

      ~RerotateOnDestroy()
      {
         if(m_remove_it){
            rotate_up_n(m_header, m_p, m_n);
         }
      }

      void release()
      {  m_remove_it = false;  }

      const NodePtr m_header;
      const NodePtr m_p;
      std::size_t &m_n;
      bool m_remove_it;
   };

   static void rotate_up_n(const NodePtr header, const NodePtr p, std::size_t n)
   {
      NodePtr p_parent(TNodeTraits::get_parent(p));
      NodePtr p_grandparent(TNodeTraits::get_parent(p_parent));
      while(n--){
         if(p == TNodeTraits::get_left(p_parent)){  //p is left child
            BstreeAlgo::rotate_right(p_parent, p, p_grandparent, header);
         }
         else{ //p is right child
            BstreeAlgo::rotate_left(p_parent, p, p_grandparent, header);
         }
         p_parent      = p_grandparent;
         p_grandparent = TNodeTraits::get_parent(p_parent);
      }
   }

   /// @endcond

   public:
   //! This type is the information that will be
   //! filled by insert_unique_check
   struct InsertCommitData
      /// @cond
      :  public BstreeAlgo::InsertCommitData
      /// @endcond
   {
      /// @cond
      inline InsertCommitData()
         : BstreeAlgo::InsertCommitData(), m_rotations()
      {}

      std::size_t m_rotations;
      /// @endcond
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
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::unlink(node_ptr)
   template<class NodePtrPriorityCompare>
   static void unlink(NodePtr n, NodePtrPriorityCompare pcomp)
   {
      NodePtr x = TNodeTraits::get_parent(n);
      if(x){
         while(!BstreeAlgo::is_header(x))
            x = TNodeTraits::get_parent(x);
         erase(x, n, pcomp);
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

   //! @copydoc ::boost::intrusive::bstree_algorithms::init_header(node_ptr)
   static void init_header(node_ptr header) BOOST_NOEXCEPT;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::erase(node_ptr,node_ptr)
   template<class NodePtrPriorityCompare>
   static NodePtr erase(NodePtr header, NodePtr z, NodePtrPriorityCompare pcomp)
   {
      rebalance_for_erasure(header, z, pcomp);
      BstreeAlgo::erase(header, z);
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

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the upper bound
   //!   according to "comp" and rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throw or "pcomp" throw.
   template<class NodePtrCompare, class NodePtrPriorityCompare>
   static NodePtr insert_equal_upper_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::insert_equal_upper_bound_check(h, new_node, comp, commit_data);
      rebalance_check_and_commit(h, new_node, pcomp, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "h" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before the upper bound
   //!   according to "comp" and rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If "comp" throws.
   template<class NodePtrCompare, class NodePtrPriorityCompare>
   static NodePtr insert_equal_lower_bound
      (NodePtr h, NodePtr new_node, NodePtrCompare comp, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::insert_equal_lower_bound_check(h, new_node, comp, commit_data);
      rebalance_check_and_commit(h, new_node, pcomp, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   NodePtrCompare is a function object that induces a strict weak
   //!   ordering compatible with the strict weak ordering used to create the
   //!   the tree. NodePtrCompare compares two node_ptrs. "hint" is node from
   //!   the "header"'s tree.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case).
   //!   Rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if new_node is inserted immediately before "hint".
   //!
   //! <b>Throws</b>: If "comp" throw or "pcomp" throw.
   template<class NodePtrCompare, class NodePtrPriorityCompare>
   static NodePtr insert_equal
      (NodePtr h, NodePtr hint, NodePtr new_node, NodePtrCompare comp, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::insert_equal_check(h, hint, new_node, comp, commit_data);
      rebalance_check_and_commit(h, new_node, pcomp, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "pos" must be a valid node of the tree (including header end) node.
   //!   "pos" must be a node pointing to the successor to "new_node"
   //!   once inserted according to the order of already inserted nodes. This function does not
   //!   check "pos" and this precondition must be guaranteed by the caller.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts new_node into the tree before "pos"
   //!   and rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: If "pcomp" throws, strong guarantee.
   //!
   //! <b>Note</b>: If "pos" is not the successor of the newly inserted "new_node"
   //! tree invariants might be broken.
   template<class NodePtrPriorityCompare>
   static NodePtr insert_before
      (NodePtr header, NodePtr pos, NodePtr new_node, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::insert_before_check(header, pos, commit_data);
      rebalance_check_and_commit(header, new_node, pcomp, commit_data);
      return new_node;
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering no less than the
   //!   greatest inserted key.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts x into the tree in the last position
   //!   and rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: If "pcomp" throws, strong guarantee.
   //!
   //! <b>Note</b>: If "new_node" is less than the greatest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   template<class NodePtrPriorityCompare>
   static void push_back(NodePtr header, NodePtr new_node, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::push_back_check(header, commit_data);
      rebalance_check_and_commit(header, new_node, pcomp, commit_data);
   }

   //! <b>Requires</b>: "header" must be the header node of a tree.
   //!   "new_node" must be, according to the used ordering, no greater than the
   //!   lowest inserted key.
   //!   NodePtrPriorityCompare is a priority function object that induces a strict weak
   //!   ordering compatible with the one used to create the
   //!   the tree. NodePtrPriorityCompare compares two node_ptrs.
   //!
   //! <b>Effects</b>: Inserts x into the tree in the first position
   //!   and rotates the tree according to "pcomp".
   //!
   //! <b>Complexity</b>: Constant-time.
   //!
   //! <b>Throws</b>: If "pcomp" throws, strong guarantee.
   //!
   //! <b>Note</b>: If "new_node" is greater than the lowest inserted key
   //! tree invariants are broken. This function is slightly faster than
   //! using "insert_before".
   template<class NodePtrPriorityCompare>
   static void push_front(NodePtr header, NodePtr new_node, NodePtrPriorityCompare pcomp)
   {
      InsertCommitData commit_data;
      BstreeAlgo::push_front_check(header, commit_data);
      rebalance_check_and_commit(header, new_node, pcomp, commit_data);
   }

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
   template<class KeyType, class KeyNodePtrCompare, class PrioType, class PrioNodePtrPrioCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      ( ConstNodePtr header
      , const KeyType &key, KeyNodePtrCompare comp
      , const PrioType &prio, PrioNodePtrPrioCompare pcomp
      , InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         BstreeAlgo::insert_unique_check(header, key, comp, commit_data);
      if(ret.second)
         rebalance_after_insertion_check(header, commit_data.node, prio, pcomp, commit_data.m_rotations);
      return ret;
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
   template<class KeyType, class KeyNodePtrCompare, class PrioType, class PrioNodePtrPrioCompare>
   static std::pair<NodePtr, bool> insert_unique_check
      ( ConstNodePtr header, NodePtr hint
      , const KeyType &key, KeyNodePtrCompare comp
      , const PrioType &prio, PrioNodePtrPrioCompare pcomp
      , InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         BstreeAlgo::insert_unique_check(header, hint, key, comp, commit_data);
      if(ret.second)
         rebalance_after_insertion_check(header, commit_data.node, prio, pcomp, commit_data.m_rotations);
      return ret;
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
   static void insert_unique_commit
      (NodePtr header, NodePtr new_node, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      BstreeAlgo::insert_unique_commit(header, new_node, commit_data);
      rotate_up_n(header, new_node, commit_data.m_rotations);
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_unique
   template<class NodePtrCompare, class PrioNodePtrPrioCompare>
   static bool transfer_unique
      (NodePtr header1, NodePtrCompare comp, PrioNodePtrPrioCompare pcomp, NodePtr header2, NodePtr z)
   {
      InsertCommitData commit_data;
      bool const transferable = insert_unique_check(header1, z, comp, z, pcomp, commit_data).second;
      if(transferable){
         erase(header2, z, pcomp);
         insert_unique_commit(header1, z, commit_data);         
      }
      return transferable;
   }

   //! @copydoc ::boost::intrusive::bstree_algorithms::transfer_equal
   template<class NodePtrCompare, class PrioNodePtrPrioCompare>
   static void transfer_equal
      (NodePtr header1, NodePtrCompare comp, PrioNodePtrPrioCompare pcomp, NodePtr header2, NodePtr z)
   {
      InsertCommitData commit_data;
      BstreeAlgo::insert_equal_upper_bound_check(header1, z, comp, commit_data);
      rebalance_after_insertion_check(header1, commit_data.node, z, pcomp, commit_data.m_rotations);
      rebalance_for_erasure(header2, z, pcomp);
      BstreeAlgo::erase(header2, z);
      BstreeAlgo::insert_unique_commit(header1, z, commit_data);
      rotate_up_n(header1, z, commit_data.m_rotations);
   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree_algorithms::is_header
   static bool is_header(const_node_ptr p) BOOST_NOEXCEPT;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   /// @cond
   private:

   template<class NodePtrPriorityCompare>
   static void rebalance_for_erasure(NodePtr header, NodePtr z, NodePtrPriorityCompare pcomp)
   {
      std::size_t n = 0;
      RerotateOnDestroy rb(header, z, n);

      NodePtr z_left  = TNodeTraits::get_left(z);
      NodePtr z_right = TNodeTraits::get_right(z);
      while(z_left || z_right){
         const NodePtr z_parent(TNodeTraits::get_parent(z));
         if(!z_right || (z_left && pcomp(z_left, z_right))){
            BstreeAlgo::rotate_right(z, z_left, z_parent, header);
         }
         else{
            BstreeAlgo::rotate_left(z, z_right, z_parent, header);
         }
         ++n;
         z_left  = TNodeTraits::get_left(z);
         z_right = TNodeTraits::get_right(z);
      }
      rb.release();
   }

   template<class NodePtrPriorityCompare>
   static void rebalance_check_and_commit
      (NodePtr h, NodePtr new_node, NodePtrPriorityCompare pcomp, InsertCommitData &commit_data)
   {
      rebalance_after_insertion_check(h, commit_data.node, new_node, pcomp, commit_data.m_rotations);
      //No-throw
      BstreeAlgo::insert_unique_commit(h, new_node, commit_data);
      rotate_up_n(h, new_node, commit_data.m_rotations);
   }

   template<class Key, class KeyNodePriorityCompare>
   static void rebalance_after_insertion_check
      (ConstNodePtr header, ConstNodePtr up, const Key &k
      , KeyNodePriorityCompare pcomp, std::size_t &num_rotations)
   {
      ConstNodePtr upnode(up);
      //First check rotations since pcomp can throw
      num_rotations = 0;
      std::size_t n = 0;
      while(upnode != header && pcomp(k, upnode)){
         ++n;
         upnode = TNodeTraits::get_parent(upnode);
      }
      num_rotations = n;
   }

   template<class NodePtrPriorityCompare>
   static bool check_invariant(ConstNodePtr header, NodePtrPriorityCompare pcomp)
   {
      NodePtr beg = begin_node(header);
      NodePtr end = end_node(header);

      while(beg != end){
         NodePtr p = TNodeTraits::get_parent(beg);
         if(p != header){
            if(pcomp(beg, p))
               return false;
         }
         beg = next_node(beg);
      }
      return true;
   }

   /// @endcond
};

/// @cond

template<class NodeTraits>
struct get_algo<TreapAlgorithmsValue, NodeTraits>
{
   typedef TreapAlgorithms<NodeTraits> Type;
};

template <class ValueTraits, class NodePtrCompare, class ExtraChecker>
struct get_node_checker<TreapAlgorithmsValue, ValueTraits, NodePtrCompare, ExtraChecker>
{
   typedef detail::BstreeNodeChecker<ValueTraits, NodePtrCompare, ExtraChecker> Type;
};

/// @endcond

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREAP_ALGORITHMS_HPP
