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

#ifndef BOOST_INTRUSIVE_DETAIL_ARRAY_INITIALIZER_HPP
#define BOOST_INTRUSIVE_DETAIL_ARRAY_INITIALIZER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/config.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/move/detail/placement_new.hpp>
#include <boost/move/detail/force_ptr.hpp>

namespace boost {
namespace intrusive {
namespace detail {

//This is not standard, but should work with all compilers
union max_align
{
   char        m_char;
   short       m_short;
   int         m_int;
   long        m_long;
   #ifdef BOOST_HAS_LONG_LONG
   ::boost::long_long_type  m_long_long;
   #endif
   float       m_float;
   double      m_double;
   long double m_long_double;
   void *      m_void_ptr;
};

template<class T, std::size_t N>
class ArrayInitializer
{
   public:
   template<class CommonInitializer>
   ArrayInitializer(const CommonInitializer &init)
   {
      char *init_buf = (char*)m_rawbuf;
      std::size_t i = 0;
      BOOST_INTRUSIVE_TRY{
         for(; i != N; ++i){
            ::new(init_buf, boost_move_new_t()) T(init);
            init_buf += sizeof(T);
         }
      }
      BOOST_INTRUSIVE_CATCH(...){
         while(i--){
            init_buf -= sizeof(T);
            move_detail::force_ptr<T*>(init_buf)->~T();
         }
         BOOST_INTRUSIVE_RETHROW;
      }
      BOOST_INTRUSIVE_CATCH_END
   }

   operator T* ()
   {  return (T*)(m_rawbuf);  }

   operator const T*() const
   {  return (const T*)(m_rawbuf);  }

   ~ArrayInitializer()
   {
      char *init_buf = (char*)m_rawbuf + N*sizeof(T);
      for(std::size_t i = 0; i != N; ++i){
         init_buf -= sizeof(T);
         move_detail::force_ptr<T*>(init_buf)->~T();
      }
   }

   private:
   detail::max_align m_rawbuf[(N*sizeof(T)-1)/sizeof(detail::max_align)+1];
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_ARRAY_INITIALIZER_HPP
