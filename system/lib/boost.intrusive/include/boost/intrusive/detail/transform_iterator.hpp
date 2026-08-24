/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP
#define BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/iterator.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template <class PseudoReference>
struct OperatorArrowProxy
{
   inline OperatorArrowProxy(const PseudoReference &px)
      :  m_value(px)
   {}

   inline PseudoReference* operator->() const { return &m_value; }
   // This function is needed for MWCW and BCC, which won't call operator->
   // again automatically per 13.3.1.2 para 8
//   operator T*() const { return &m_value; }
   mutable PseudoReference m_value;
};

template <class T>
struct OperatorArrowProxy<T&>
{
   inline OperatorArrowProxy(T &px)
      :  m_value(px)
   {}

   inline T* operator->() const { return &m_value; }
   // This function is needed for MWCW and BCC, which won't call operator->
   // again automatically per 13.3.1.2 para 8
//   operator T*() const { return &m_value; }
   T &m_value;
};

template <class Iterator, class UnaryFunction>
class TransformIterator
{
   public:
   typedef typename Iterator::IteratorCategory                                           IteratorCategory;
   typedef typename detail::remove_reference<typename UnaryFunction::result_type>::type   ValueType;
   typedef typename Iterator::DifferenceType                                             DifferenceType;
   typedef OperatorArrowProxy<typename UnaryFunction::result_type>                      Pointer;
   typedef typename UnaryFunction::result_type                                            Reference;
   
   explicit TransformIterator(const Iterator &it, const UnaryFunction &f = UnaryFunction())
      :  m_members(it, f)
   {}

   explicit TransformIterator()
      :  m_members()
   {}

   inline Iterator get_it() const
   {  return m_members.m_it;   }

   //Constructors
   inline TransformIterator& operator++()
   { increment();   return *this;   }

   inline TransformIterator operator++(int)
   {
      TransformIterator result (*this);
      increment();
      return result;
   }

   inline friend bool operator== (const TransformIterator& i, const TransformIterator& i2)
   { return i.equal(i2); }

   inline friend bool operator!= (const TransformIterator& i, const TransformIterator& i2)
   { return !(i == i2); }

   inline friend typename Iterator::difference_type operator- (const TransformIterator& i, const TransformIterator& i2)
   { return i2.distance_to(i); }

   //Arithmetic
   TransformIterator& operator+=(typename Iterator::difference_type off)
   {  this->advance(off); return *this;   }

   inline TransformIterator operator+(typename Iterator::difference_type off) const
   {
      TransformIterator other(*this);
      other.advance(off);
      return other;
   }

   inline friend TransformIterator operator+(typename Iterator::difference_type off, const TransformIterator& right)
   {  return right + off; }

   inline TransformIterator& operator-=(typename Iterator::difference_type off)
   {  this->advance(-off); return *this;   }

   inline TransformIterator operator-(typename Iterator::difference_type off) const
   {  return *this + (-off);  }

   inline typename UnaryFunction::result_type operator*() const
   { return dereference(); }

   inline OperatorArrowProxy<typename UnaryFunction::result_type>
      operator->() const
   { return OperatorArrowProxy<typename UnaryFunction::result_type>(dereference());  }

   private:
   struct Members
      :  UnaryFunction
   {
      inline Members(const Iterator &it, const UnaryFunction &f)
         :  UnaryFunction(f), m_it(it)
      {}

      inline Members()
      {}

      Iterator m_it;
   } m_members;


   inline void increment()
   { ++m_members.m_it; }

   inline void decrement()
   { --m_members.m_it; }

   inline bool equal(const TransformIterator &other) const
   {  return m_members.m_it == other.m_members.m_it;   }

   inline bool less(const TransformIterator &other) const
   {  return other.m_members.m_it < m_members.m_it;   }

   typename UnaryFunction::result_type dereference() const
   { return m_members(*m_members.m_it); }

   void advance(typename Iterator::difference_type n)
   {  boost::intrusive::iterator_advance(m_members.m_it, n); }

   typename Iterator::difference_type distance_to(const TransformIterator &other)const
   {  return boost::intrusive::iterator_distance(other.m_members.m_it, m_members.m_it); }
};

} //namespace detail
} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_DETAIL_TRANSFORM_ITERATOR_HPP
