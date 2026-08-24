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

#ifndef BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP
#define BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<bool ConstantSize, class TSizeType, class Tag = void>
struct SizeHolder
{
   static const bool CONSTANT_TIME_SIZE = ConstantSize;
   typedef TSizeType  SizeType;

   inline TSizeType get_size() const
   {  return m_size;  }

   inline void set_size(TSizeType size)
   {  m_size = size; }

   inline void decrement()
   {  --m_size; }

   inline void increment()
   {  ++m_size; }

   inline void increase(TSizeType n)
   {  m_size += n; }

   inline void decrease(TSizeType n)
   {  m_size -= n; }

   inline void swap(SizeHolder &other)
   {  TSizeType tmp(m_size); m_size = other.m_size; other.m_size = tmp; }

   TSizeType m_size;
};

template<class TSizeType, class Tag>
struct SizeHolder<false, TSizeType, Tag>
{
   static const bool CONSTANT_TIME_SIZE = false;
   typedef TSizeType  SizeType;

   inline SizeType get_size() const
   {  return 0;  }

   inline void set_size(SizeType)
   {}

   inline void decrement()
   {}

   inline void increment()
   {}

   inline void increase(TSizeType)
   {}

   inline void decrease(TSizeType)
   {}

   inline void swap(SizeHolder){}
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_SIZE_HOLDER_HPP
