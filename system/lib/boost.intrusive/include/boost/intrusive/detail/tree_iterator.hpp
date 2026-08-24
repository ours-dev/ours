/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_TREE_ITERATOR_HPP
#define BOOST_INTRUSIVE_TREE_ITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/std_fwd.hpp>
#include <boost/intrusive/detail/iiterator.hpp>
#include <boost/intrusive/detail/bstree_algorithms_base.hpp>

namespace boost {
namespace intrusive {

/////////////////////////////////////////////////////////////////////////////
//                                                                         //
//                   Implementation of the tree iterator                   //
//                                                                         //
/////////////////////////////////////////////////////////////////////////////

// tree_iterator provides some basic functions for a
// node oriented bidirectional iterator:
template<class TValueTraits, bool IsConst>
class TreeIterator
{
   private:
   typedef Iiterator< TValueTraits, IsConst
                    , std::bidirectional_iterator_tag>   TypesT;
   typedef typename TypesT::ValueTraits                ValueTraits;
   typedef typename TypesT::NodeTraits                 NodeTraits;
   typedef typename TypesT::Node                        Node;
   typedef typename TypesT::NodePtr                    NodePtr;
   typedef typename TypesT::ConstValueTraitsPtr      ConstValueTraitsPtr;
   typedef BstreeAlgorithmsBase<NodeTraits>           NodeAlgorithms;

   static const bool STATEFUL_VALUE_TRAITS = TypesT::STATEFUL_VALUE_TRAITS;

   void unspecified_bool_type_func() const {}
   typedef void (TreeIterator::*UnspecifiedBoolType)() const;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , TreeIterator<ValueTraits, false>
                  , nat>::type                           NonconstIterator;

   public:
   typedef typename TypesT::iterator_type::DifferenceType    DifferenceType;
   typedef typename TypesT::iterator_type::ValueType         ValueType;
   typedef typename TypesT::iterator_type::Pointer            Pointer;
   typedef typename TypesT::iterator_type::Reference          Reference;
   typedef typename TypesT::iterator_type::IteratorCategory  IteratorCategory;

   inline TreeIterator()
   {}

   inline explicit TreeIterator(NodePtr nodeptr, ConstValueTraitsPtr traits_ptr)
      : m_members(nodeptr, traits_ptr)
   {}

   inline TreeIterator(const TreeIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline TreeIterator(const NonconstIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline TreeIterator &operator=(const TreeIterator &other)
   {  m_members.nodeptr_ = other.m_members.nodeptr_;  return *this;  }

   inline TreeIterator &operator=(NodePtr nodeptr)
   {  m_members.nodeptr_ = nodeptr;  return *this;  }

   inline NodePtr pointed_node() const
   { return m_members.nodeptr_; }

   public:
   inline TreeIterator& operator++()
   {
      m_members.nodeptr_ = NodeAlgorithms::next_node(m_members.nodeptr_);
      return *this;
   }

   TreeIterator operator++(int)
   {
      TreeIterator result (*this);
      m_members.nodeptr_ = NodeAlgorithms::next_node(m_members.nodeptr_);
      return result;
   }

   inline TreeIterator& operator--()
   {
      m_members.nodeptr_ = NodeAlgorithms::prev_node(m_members.nodeptr_);
      return *this;
   }

   TreeIterator operator--(int)
   {
      TreeIterator result (*this);
      m_members.nodeptr_ = NodeAlgorithms::prev_node(m_members.nodeptr_);
      return result;
   }

   inline TreeIterator&  go_left()
   {
      m_members.nodeptr_ = NodeTraits::get_left(m_members.nodeptr_);
      return *this;
   }

   inline TreeIterator&  go_right()
   {
      m_members.nodeptr_ = NodeTraits::get_right(m_members.nodeptr_);
      return *this;
   }

   inline TreeIterator&  go_parent()
   {
      m_members.nodeptr_ = NodeTraits::get_parent(m_members.nodeptr_);
      return *this;
   }

   inline operator UnspecifiedBoolType() const
   {  return m_members.nodeptr_ ? &TreeIterator::unspecified_bool_type_func : 0;   }

   inline bool operator! () const
   {  return !m_members.nodeptr_;   }

   inline friend bool operator== (const TreeIterator& l, const TreeIterator& r)
   { return l.pointed_node() == r.pointed_node(); }

   inline friend bool operator!= (const TreeIterator& l, const TreeIterator& r)
   {  return !(l == r);   }

   inline Reference operator*() const
   {  return *operator->();   }

   inline Pointer operator->() const
   { return this->operator_arrow(detail::bool_<STATEFUL_VALUE_TRAITS>()); }

   inline ConstValueTraitsPtr get_value_traits() const
   {  return m_members.get_ptr();  }

   TreeIterator end_iterator_from_it() const
   {
      return TreeIterator(NodeAlgorithms::get_header(this->pointed_node()), this->get_value_traits());
   }

   TreeIterator<ValueTraits, false> unconst() const
   {  return TreeIterator<ValueTraits, false>(this->pointed_node(), this->get_value_traits());   }

   private:
   inline Pointer operator_arrow(detail::false_) const
   { return TValueTraits::to_value_ptr(m_members.nodeptr_); }

   inline Pointer operator_arrow(detail::true_) const
   { return this->get_value_traits()->to_value_ptr(m_members.nodeptr_); }

   IiteratorMembers<NodePtr, ConstValueTraitsPtr, STATEFUL_VALUE_TRAITS> m_members;
};

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREE_ITERATOR_HPP
