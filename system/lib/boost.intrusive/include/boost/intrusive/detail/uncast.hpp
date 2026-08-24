/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_UNCAST_HPP
#define BOOST_INTRUSIVE_DETAIL_UNCAST_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<class ConstNodePtr>
struct UncastTypes
{
   typedef typename PointerTraits<ConstNodePtr>::ElementType ElementType;
   typedef typename remove_const<ElementType>::type           NonConstType;
   typedef typename PointerTraits<ConstNodePtr>::
      template rebind_pointer<NonConstType>::type            NonConstPointer;
   typedef PointerTraits<NonConstPointer>                   NonConstTraits;
};

template<class ConstNodePtr>
static typename UncastTypes<ConstNodePtr>::non_const_pointer
   uncast(const ConstNodePtr & ptr)
{
   return UncastTypes<ConstNodePtr>::non_const_traits::const_cast_from(ptr);
}

} //namespace detail {
} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_DETAIL_UTILITIES_HPP
