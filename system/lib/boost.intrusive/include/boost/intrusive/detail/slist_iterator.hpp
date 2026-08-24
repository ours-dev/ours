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

#ifndef BOOST_INTRUSIVE_SLIST_ITERATOR_HPP
#define BOOST_INTRUSIVE_SLIST_ITERATOR_HPP

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
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {


// slist_iterator provides some basic functions for a
// node oriented bidirectional iterator:
template<class TValueTraits, bool IsConst>
class SlistIterator
{
   private:
   typedef Iiterator
      <TValueTraits, IsConst, std::forward_iterator_tag> TypesT;

   static const bool STATEFUL_VALUE_TRAITS =                TypesT::STATEFUL_VALUE_TRAITS;

   typedef TValueTraits                                      ValueTraits;
   typedef typename TypesT::NodeTraits                    NodeTraits;

   typedef typename TypesT::Node                           Node;
   typedef typename TypesT::NodePtr                       NodePtr;
   typedef typename TypesT::ConstValueTraitsPtr         ConstValueTraitsPtr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , SlistIterator<ValueTraits, false>
                  , nat>::type                              NonconstIterator;

   public:
   typedef typename TypesT::iterator_type::DifferenceType    DifferenceType;
   typedef typename TypesT::iterator_type::ValueType         ValueType;
   typedef typename TypesT::iterator_type::Pointer            Pointer;
   typedef typename TypesT::iterator_type::Reference          Reference;
   typedef typename TypesT::iterator_type::IteratorCategory  IteratorCategory;

   inline SlistIterator()
   {}

   inline SlistIterator(NodePtr nodeptr, ConstValueTraitsPtr traits_ptr)
      : m_members(nodeptr, traits_ptr)
   {}

   inline explicit SlistIterator(NodePtr nodeptr)
      : m_members(nodeptr, ConstValueTraitsPtr())
   {  BOOST_INTRUSIVE_STATIC_ASSERT((STATEFUL_VALUE_TRAITS == false));  }

   inline SlistIterator(const SlistIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline SlistIterator(const NonconstIterator &other)
      :  m_members(other.pointed_node(), other.get_value_traits())
   {}

   inline SlistIterator &operator=(const SlistIterator &other)
   {  m_members.nodeptr_ = other.m_members.nodeptr_;  return *this;  }

   inline NodePtr pointed_node() const
   { return m_members.nodeptr_; }

   inline SlistIterator &operator=(NodePtr n)
   {  m_members.nodeptr_ = n;  return static_cast<SlistIterator&>(*this);  }

   inline ConstValueTraitsPtr get_value_traits() const
   {  return m_members.get_ptr(); }

   inline bool operator!() const
   {  return !m_members.nodeptr_; }

   public:
   inline SlistIterator& operator++()
   {
      m_members.nodeptr_ = NodeTraits::get_next(m_members.nodeptr_);
      return static_cast<SlistIterator&> (*this);
   }

   inline SlistIterator operator++(int)
   {
      SlistIterator result (*this);
      m_members.nodeptr_ = NodeTraits::get_next(m_members.nodeptr_);
      return result;
   }

   inline friend bool operator== (const SlistIterator& l, const SlistIterator& r)
   {  return l.pointed_node() == r.pointed_node();   }

   inline friend bool operator!= (const SlistIterator& l, const SlistIterator& r)
   {  return l.pointed_node() != r.pointed_node();   }

   inline Reference operator*() const
   {  return *operator->();   }

   inline Pointer operator->() const
   { return this->operator_arrow(detail::bool_<STATEFUL_VALUE_TRAITS>()); }

   inline SlistIterator<TValueTraits, false> unconst() const
   {  return SlistIterator<TValueTraits, false>(this->pointed_node(), this->get_value_traits());   }

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

#endif //BOOST_INTRUSIVE_SLIST_ITERATOR_HPP
