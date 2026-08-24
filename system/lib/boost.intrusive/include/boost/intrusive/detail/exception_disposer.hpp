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

#ifndef BOOST_INTRUSIVE_DETAIL_EXCEPTION_DISPOSER_HPP
#define BOOST_INTRUSIVE_DETAIL_EXCEPTION_DISPOSER_HPP

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

template<class Container, class Disposer>
class ExceptionDisposer
{
   Container *m_cont;
   Disposer  &m_disp;

   ExceptionDisposer(const ExceptionDisposer&);
   ExceptionDisposer &operator=(const ExceptionDisposer&);

   public:
   ExceptionDisposer(Container &cont, Disposer &disp)
      :  m_cont(&cont), m_disp(disp)
   {}

   inline void release()
   {  m_cont = 0;  }

   ~ExceptionDisposer()
   {
      if(m_cont){
         m_cont->clear_and_dispose(m_disp);
      }
   }
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_EXCEPTION_DISPOSER_HPP
