/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Joaquin M Lopez Munoz  2006-2013
// (C) Copyright Ion Gaztanaga          2014-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_EBO_HOLDER_HPP
#define BOOST_INTRUSIVE_DETAIL_EBO_HOLDER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/move/utility_core.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<typename T, typename Tag = void>
class EboFunctorHolder
   :  public T
{
   BOOST_COPYABLE_AND_MOVABLE(EboFunctorHolder)

   public:
   typedef T FunctorType;

   inline EboFunctorHolder()
      : T()
   {}

   inline explicit EboFunctorHolder(const T &t)
      : T(t)
   {}

   inline explicit EboFunctorHolder(BOOST_RV_REF(T) t)
      : T(::boost::move(t))
   {}

   template<class Arg1, class Arg2>
   inline EboFunctorHolder(BOOST_FWD_REF(Arg1) arg1, BOOST_FWD_REF(Arg2) arg2)
      : T(::boost::forward<Arg1>(arg1), ::boost::forward<Arg2>(arg2))
   {}

   inline EboFunctorHolder(const EboFunctorHolder &x)
      : T(static_cast<const T&>(x))
   {}

   inline EboFunctorHolder(BOOST_RV_REF(EboFunctorHolder) x)
      : T(BOOST_MOVE_BASE(T, x))
   {}

   inline EboFunctorHolder& operator=(BOOST_COPY_ASSIGN_REF(EboFunctorHolder) x)
   {
      const EboFunctorHolder&r = x;
      this->get() = r;
      return *this;
   }

   inline EboFunctorHolder& operator=(BOOST_RV_REF(EboFunctorHolder) x)
   {
      this->get() = ::boost::move(x.get());
      return *this;
   }

   inline EboFunctorHolder& operator=(const T &t)
   {
      this->get() = t;
      return *this;
   }

   inline EboFunctorHolder& operator=(BOOST_RV_REF(T) t)
   {
      this->get() = ::boost::move(t);
      return *this;
   }

   inline T&       get(){return *this;}
   inline const T& get()const{return *this;}
};

template<typename T, typename Tag>
class EboFunctorHolder<T *, Tag>
{
   BOOST_COPYABLE_AND_MOVABLE(EboFunctorHolder)

   public:
   typedef T FunctorType;

   inline EboFunctorHolder()
      : m_t()
   {}

   inline explicit EboFunctorHolder(T * t)
      : m_t(t)
   {}

   inline EboFunctorHolder(const EboFunctorHolder &x)
      : m_t(x.m_t)
   {}

   inline EboFunctorHolder(BOOST_RV_REF(EboFunctorHolder) x)
      : m_t(x.m_t)
   {}

   inline EboFunctorHolder& operator=(BOOST_COPY_ASSIGN_REF(EboFunctorHolder) x)
   {
      this->m_t = x.m_t;
      return *this;
   }

   inline EboFunctorHolder& operator=(BOOST_RV_REF(EboFunctorHolder) x)
   {
      this->m_t = ::boost::move(x.m_t);
      return *this;
   }

   inline EboFunctorHolder& operator=(T * t)
   {
      this->m_t = t;
      return *this;
   }

   inline T&       get(){return *m_t;}

   private:
   T * m_t;
};

}  //namespace detail {
}  //namespace intrusive {
}  //namespace boost {

#endif   //#ifndef BOOST_INTRUSIVE_DETAIL_EBO_HOLDER_HPP
