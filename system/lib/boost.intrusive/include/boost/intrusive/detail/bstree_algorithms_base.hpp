/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_BSTREE_ALGORITHMS_BASE_HPP
#define BOOST_INTRUSIVE_BSTREE_ALGORITHMS_BASE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/uncast.hpp>

namespace boost {
namespace intrusive {

template<class TNodeTraits>
class BstreeAlgorithmsBase
{
   public:
   typedef typename TNodeTraits::Node            Node;
   typedef TNodeTraits                           NodeTraits;
   typedef typename TNodeTraits::NodePtr        NodePtr;
   typedef typename TNodeTraits::ConstNodePtr  ConstNodePtr;

   //! <b>Requires</b>: 'n' is a node from the tree except the header.
   //!
   //! <b>Effects</b>: Returns the next node of the tree.
   //!
   //! <b>Complexity</b>: Average constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr next_node(NodePtr n) BOOST_NOEXCEPT
   {
      NodePtr const n_right(TNodeTraits::get_right(n));
      if(n_right){
         return minimum(n_right);
      }
      else {
         NodePtr p(TNodeTraits::get_parent(n));
         while(n == TNodeTraits::get_right(p)){
            n = p;
            p = TNodeTraits::get_parent(p);
         }
         return TNodeTraits::get_right(n) != p ? p : n;
      }
   }

   //! <b>Requires</b>: 'n' is a node from the tree except the leftmost node.
   //!
   //! <b>Effects</b>: Returns the previous node of the tree.
   //!
   //! <b>Complexity</b>: Average constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr prev_node(NodePtr n) BOOST_NOEXCEPT
   {
      if(is_header(n)){
         return TNodeTraits::get_right(n);
      }
      else if(TNodeTraits::get_left(n)){
         return maximum(TNodeTraits::get_left(n));
      }
      else {
         NodePtr p(n);
         NodePtr x = TNodeTraits::get_parent(p);
         while(p == TNodeTraits::get_left(x)){
            p = x;
            x = TNodeTraits::get_parent(x);
         }
         return x;
      }
   }

   //! <b>Requires</b>: 'n' is a node of a tree but not the header.
   //!
   //! <b>Effects</b>: Returns the minimum node of the subtree starting at p.
   //!
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr minimum(NodePtr n)
   {
      for(NodePtr p_left = TNodeTraits::get_left(n)
         ;p_left
         ;p_left = TNodeTraits::get_left(n)){
         n = p_left;
      }
      return n;
   }

   //! <b>Requires</b>: 'n' is a node of a tree but not the header.
   //!
   //! <b>Effects</b>: Returns the maximum node of the subtree starting at p.
   //!
   //! <b>Complexity</b>: Logarithmic to the size of the subtree.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr maximum(NodePtr n)
   {
      for(NodePtr p_right = TNodeTraits::get_right(n)
         ;p_right
         ;p_right = TNodeTraits::get_right(n)){
         n = p_right;
      }
      return n;
   }

   //! <b>Requires</b>: p is a node of a tree.
   //!
   //! <b>Effects</b>: Returns true if p is the header of the tree.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   static bool is_header(ConstNodePtr p) BOOST_NOEXCEPT
   {
      NodePtr p_left (TNodeTraits::get_left(p));
      NodePtr p_right(TNodeTraits::get_right(p));
      if(!TNodeTraits::get_parent(p) || //Header condition when empty tree
         (p_left && p_right &&         //Header always has leftmost and rightmost
            (p_left == p_right ||      //Header condition when only node
               (TNodeTraits::get_parent(p_left)  != p ||
                TNodeTraits::get_parent(p_right) != p ))
               //When tree size > 1 headers can't be leftmost's
               //and rightmost's parent
          )){
         return true;
      }
      return false;
   }

   //! <b>Requires</b>: 'n' is a node of the tree or a header node.
   //!
   //! <b>Effects</b>: Returns the header of the tree.
   //!
   //! <b>Complexity</b>: Logarithmic.
   //!
   //! <b>Throws</b>: Nothing.
   static NodePtr get_header(ConstNodePtr n)
   {
      NodePtr nn(detail::uncast(n));
      NodePtr p(TNodeTraits::get_parent(n));
      //If p is null, then nn is the header of an empty tree
      if(p){
         //Non-empty tree, check if nn is neither root nor header
         NodePtr pp(TNodeTraits::get_parent(p));
         //If granparent is not equal to nn, then nn is neither root nor header,
         //the try the fast path
         if(nn != pp){
            do{
               nn = p;
               p = pp;
               pp = TNodeTraits::get_parent(pp);
            }while(nn != pp);
            nn = p;
         }
         //Check if nn is root or header when size() > 0
         else if(!BstreeAlgorithmsBase::is_header(nn)){
            nn = p;
         }
      }
      return nn;
   }
};

}  //namespace intrusive
}  //namespace boost

#endif //BOOST_INTRUSIVE_BSTREE_ALGORITHMS_BASE_HPP
