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

#ifndef BOOST_INTRUSIVE_COMMON_SLIST_ALGORITHMS_HPP
#define BOOST_INTRUSIVE_COMMON_SLIST_ALGORITHMS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <cstddef>

namespace boost {
namespace intrusive {
namespace detail {

template<class TNodeTraits>
class CommonSlistAlgorithms
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;
   typedef TNodeTraits                           NodeTraits;

   static NodePtr get_previous_node(NodePtr p, NodePtr this_node)
   {
      for( NodePtr p_next
         ; this_node != (p_next = TNodeTraits::get_next(p))
         ; p = p_next){
         //Logic error: possible use of linear lists with
         //operations only permitted with circular lists
         BOOST_INTRUSIVE_INVARIANT_ASSERT(p);
      }
      return p;
   }

   inline static void init(NodePtr this_node) BOOST_NOEXCEPT
   {  TNodeTraits::set_next(this_node, NodePtr());  }

   static bool unique(ConstNodePtr this_node) BOOST_NOEXCEPT
   {
      NodePtr next = TNodeTraits::get_next(this_node);
      return !next || next == this_node;
   }

   inline static bool inited(ConstNodePtr this_node) BOOST_NOEXCEPT
   {  return !TNodeTraits::get_next(this_node); }

   inline static void unlink_after(NodePtr prev_node) BOOST_NOEXCEPT
   {
      ConstNodePtr this_node(TNodeTraits::get_next(prev_node));
      TNodeTraits::set_next(prev_node, TNodeTraits::get_next(this_node));
   }

   inline static void unlink_after(NodePtr prev_node, NodePtr last_node) BOOST_NOEXCEPT
   {  TNodeTraits::set_next(prev_node, last_node);  }

   static void link_after(NodePtr prev_node, NodePtr this_node) BOOST_NOEXCEPT
   {
      TNodeTraits::set_next(this_node, TNodeTraits::get_next(prev_node));
      TNodeTraits::set_next(prev_node, this_node);
   }

   static void incorporate_after(NodePtr bp, NodePtr b, NodePtr be) BOOST_NOEXCEPT
   {
      NodePtr p(TNodeTraits::get_next(bp));
      TNodeTraits::set_next(bp, b);
      TNodeTraits::set_next(be, p);
   }

   static void transfer_after(NodePtr bp, NodePtr bb, NodePtr be) BOOST_NOEXCEPT
   {
      if (bp != bb && bp != be && bb != be) {
         NodePtr next_b = TNodeTraits::get_next(bb);
         NodePtr next_e = TNodeTraits::get_next(be);
         NodePtr next_p = TNodeTraits::get_next(bp);
         TNodeTraits::set_next(bb, next_e);
         TNodeTraits::set_next(be, next_p);
         TNodeTraits::set_next(bp, next_b);
      }
   }

   struct StablePartitionInfo
   {
      std::size_t m_num_1st_partition;
      std::size_t m_num_2nd_partition;
      NodePtr    m_beg_2st_partition;
      NodePtr    m_new_last_node;
   };

   template<class Pred>
   static void stable_partition(NodePtr before_beg, NodePtr end, Pred pred, StablePartitionInfo &info)
   {
      NodePtr bcur = before_beg;
      NodePtr cur  = NodeTraits::get_next(bcur);
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
                     NodeTraits::set_next(last_to_remove, cur);
                     last_to_remove = cur;
                     NodePtr nxt = NodeTraits::get_next(cur);
                     NodeTraits::set_next(bcur, nxt);
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
               NodeTraits::set_next(last_to_remove, new_f);
               BOOST_INTRUSIVE_RETHROW;
            }
            BOOST_INTRUSIVE_CATCH_END
            NodeTraits::set_next(last_to_remove, new_f);
            break;
         }
      }
      info.m_num_1st_partition = num1;
      info.m_num_2nd_partition = num2;
      info.m_beg_2st_partition = new_f;
      info.m_new_last_node = bcur;
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
      ConstNodePtr i(f);
      std::size_t result = 0;
      while(i != l){
         i = TNodeTraits::get_next(i);
         ++result;
      }
      return result;
   }

   //! <b>Requires</b>: "disposer" must be an object function
   //!   taking a node_ptr parameter and shouldn't throw.
   //!
   //! <b>Effects</b>: Calls
   //!   <tt>void disposer::operator()(node_ptr)</tt> for every node of the list
   //!    [p, e).
   //!
   //! <b>Returns</b>: The number of unlinked/disposed nodes
   //!
   //! <b>Complexity</b>: Linear to the number of element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   template<class Disposer>
   static std::size_t unlink_after_and_dispose(NodePtr bb, NodePtr e, Disposer disposer) BOOST_NOEXCEPT
   {
      std::size_t n = 0u;
      NodePtr i = NodeTraits::get_next(bb);
      while (i != e) {
         NodePtr to_erase(i);
         i = NodeTraits::get_next(i);
         disposer(to_erase);
         ++n;
      }
      NodeTraits::set_next(bb, e);
      return n;
   }

   //! <b>Requires</b>: "disposer" must be an object function
   //!   taking a node_ptr parameter and shouldn't throw.
   //!
   //! <b>Effects</b>: Calls
   //!   <tt>void disposer::operator()(node_ptr)</tt> for every node of the list
   //!    after p (but not for p). Works for circular or linear lists
   //!
   //! <b>Complexity</b>: Linear to the number of element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   template<class Disposer>
   inline static void unlink_after_and_dispose(NodePtr bb, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr i = NodeTraits::get_next(bb);
      NodeTraits::set_next(bb, NodeTraits::get_next(i));
      disposer(i);
   }

   //! <b>Requires</b>: "disposer" must be an object function
   //!   taking a node_ptr parameter and shouldn't throw.
   //!
   //! <b>Effects</b>: Unlinks all nodes reachable from p (but not p) and calls
   //!   <tt>void disposer::operator()(node_ptr)</tt> for every node of the list
   //!    where p is linked.
   //!
   //! <b>Complexity</b>: Linear to the number of element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   template<class Disposer>
   static std::size_t detach_and_dispose(NodePtr p, Disposer disposer) BOOST_NOEXCEPT
   {
      std::size_t n = 0;
      NodePtr i = NodeTraits::get_next(p);
      while ( i != p || i != NodePtr() ) {
         NodePtr to_erase(i);
         i = NodeTraits::get_next(i);
         disposer(to_erase);
      }
      NodeTraits::set_next(p, i);
      return n;
   }
};

/// @endcond

} //namespace detail

/// @cond

template<class NodeTraits>
struct get_algo<CommonSListAlgorithmsValue, NodeTraits>
{
   typedef detail::CommonSlistAlgorithms<NodeTraits> Type;
};


} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_COMMON_SLIST_ALGORITHMS_HPP
