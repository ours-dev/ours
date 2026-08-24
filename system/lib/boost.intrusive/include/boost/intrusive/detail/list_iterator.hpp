/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_LIST_ITERATOR_HPP
#define BOOST_INTRUSIVE_LIST_ITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/std_fwd.hpp>
#include <boost/intrusive/detail/iiterator.hpp>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {

// list_iterator provides some basic functions for a
// node oriented bidirectional iterator:
template<class TValueTraits, bool IsConst>
class ListIterator
{
   private:
   typedef Iiterator
      <TValueTraits, IsConst, std::bidirectional_iterator_tag> TypesT;

   static const bool STATEFUL_VALUE_TRAITS =                TypesT::STATEFUL_VALUE_TRAITS;

   typedef TValueTraits                                      ValueTraits;
   typedef typename TypesT::NodeTraits                    NodeTraits;

   typedef typename TypesT::Node                           Node;
   typedef typename TypesT::NodePtr                       NodePtr;
   typedef typename TypesT::ConstValueTraitsPtr         ConstValueTraitsPtr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , ListIterator<ValueTraits, false>
                  , nat>::type                              NonconstIterator;

   public:
   typedef typename TypesT::iterator_type::DifferenceType    DifferenceType;
   typedef typename TypesT::iterator_type::ValueType         ValueType;
   typedef typename TypesT::iterator_type::Pointer            Pointer;
   typedef typename TypesT::iterator_type::Reference          Reference;
   typedef typename TypesT::iterator_type::IteratorCategory  IteratorCategory;

   inline ListIterator()
   {}

   inline explicit ListIterator(NodePtr nodeptr, ConstValueTraitsPtr traits_ptr)
      : m_members(nodeptr, traits_ptr)
   {}

   inline ListIterator(const ListIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline ListIterator(const NonconstIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline ListIterator &operator=(const ListIterator &other)
   {  m_members.nodeptr_ = other.m_members.nodeptr_;  return *this;  }

   inline NodePtr pointed_node() const
   { return m_members.nodeptr_; }

   inline ListIterator &operator=(NodePtr nodeptr)
   {  m_members.nodeptr_ = nodeptr;  return *this;  }

   inline ConstValueTraitsPtr get_value_traits() const
   {  return m_members.get_ptr(); }

   public:
   inline ListIterator& operator++()
   {
      NodePtr p = NodeTraits::get_next(m_members.nodeptr_);
      m_members.nodeptr_ = p;
      return static_cast<ListIterator&> (*this);
   }

   inline ListIterator operator++(int)
   {
      ListIterator result (*this);
      m_members.nodeptr_ = NodeTraits::get_next(m_members.nodeptr_);
      return result;
   }

   inline ListIterator& operator--()
   {
      m_members.nodeptr_ = NodeTraits::get_previous(m_members.nodeptr_);
      return static_cast<ListIterator&> (*this);
   }

   inline ListIterator operator--(int)
   {
      ListIterator result (*this);
      m_members.nodeptr_ = NodeTraits::get_previous(m_members.nodeptr_);
      return result;
   }

   inline friend bool operator== (const ListIterator& l, const ListIterator& r)
   {  return l.pointed_node() == r.pointed_node();   }

   inline friend bool operator!= (const ListIterator& l, const ListIterator& r)
   {  return !(l == r); }

   inline Reference operator*() const
   {  return *operator->();   }

   inline Pointer operator->() const
   { return this->operator_arrow(detail::bool_<STATEFUL_VALUE_TRAITS>()); }

   inline ListIterator<TValueTraits, false> unconst() const
   {  return ListIterator<TValueTraits, false>(this->pointed_node(), this->get_value_traits());   }

   private:
   inline Pointer operator_arrow(detail::false_) const
   { return TValueTraits::to_value_ptr(m_members.nodeptr_); }

   inline Pointer operator_arrow(detail::true_) const
   { return this->get_value_traits()->to_value_ptr(m_members.nodeptr_); }

   IiteratorMembers<NodePtr, ConstValueTraitsPtr, STATEFUL_VALUE_TRAITS> m_members;
};

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_LIST_ITERATOR_HPP
