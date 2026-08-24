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

#ifndef BOOST_INTRUSIVE_DETAIL_NODE_TO_VALUE_HPP
#define BOOST_INTRUSIVE_DETAIL_NODE_TO_VALUE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<class VoidPointer>
struct DummyConstptr
{
   typedef typename boost::intrusive::PointerTraits<VoidPointer>::
      template rebind_pointer<const void>::type ConstVoidPtr;

   explicit DummyConstptr(ConstVoidPtr)
   {}

   DummyConstptr()
   {}

   ConstVoidPtr get_ptr() const
   {  return ConstVoidPtr();  }
};

template<class VoidPointer>
struct Constptr
{
   typedef typename boost::intrusive::PointerTraits<VoidPointer>::
      template rebind_pointer<const void>::type ConstVoidPtr;

   Constptr()
   {}

   explicit Constptr(const ConstVoidPtr &ptr)
      :  m_const_void_ptr(ptr)
   {}

   [[nodiscard]] const void *get_ptr() const
   {  return boost::movelib::to_raw_pointer(m_const_void_ptr);  }

   ConstVoidPtr m_const_void_ptr;
};

template <class VoidPointer, bool StorePtr>
struct SelectConstptr
{
   typedef typename if_c
      < StorePtr
      , Constptr<VoidPointer>
      , DummyConstptr<VoidPointer>
      >::Type Type;
};


template<class TValueTraits, bool IsConst>
struct NodeToValue
   :  public SelectConstptr
      < typename PointerTraits
            <typename TValueTraits::pointer>::template rebind_pointer<void>::type
      , IsStatefulValueTraits<TValueTraits>::value
      >::type
{
   static const bool STATEFUL_VALUE_TRAITS = IsStatefulValueTraits<TValueTraits>::value;
   typedef typename SelectConstptr
      < typename PointerTraits
            <typename TValueTraits::pointer>::
               template rebind_pointer<void>::type
      , STATEFUL_VALUE_TRAITS >::type                 Base;

   typedef TValueTraits                                 ValueTraits;
   typedef typename ValueTraits::ValueType           ValueType;
   typedef typename ValueTraits::node_traits::Node    Node;
   typedef typename add_const_if_c
         <ValueType, IsConst>::type                   Vtype;
   typedef typename add_const_if_c
         <Node, IsConst>::type                         Ntype;
   typedef typename PointerTraits
      <typename TValueTraits::pointer>::
         template rebind_pointer<Ntype>::type          Npointer;
   typedef typename PointerTraits<Npointer>::
      template rebind_pointer<const TValueTraits>::type ConstValueTraitsPtr;

   NodeToValue(ConstValueTraitsPtr ptr)
      :  Base(ptr)
   {}

   typedef Vtype &                                 ResultType;
   typedef Ntype &                                 FirstArgumentType;

   ConstValueTraitsPtr get_value_traits() const
   {  return PointerTraits<ConstValueTraitsPtr>::static_cast_from(Base::get_ptr());  }

   ResultType to_value(FirstArgumentType arg, false_) const
   {  return *(ValueTraits::to_value_ptr(PointerTraits<Npointer>::pointer_to(arg)));  }

   ResultType to_value(FirstArgumentType arg, true_) const
   {  return *(this->get_value_traits()->to_value_ptr(PointerTraits<Npointer>::pointer_to(arg))); }

   ResultType operator()(FirstArgumentType arg) const
   {  return this->to_value(arg, bool_<STATEFUL_VALUE_TRAITS>()); }
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_NODE_TO_VALUE_HPP
