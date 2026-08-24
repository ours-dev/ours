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

#ifndef BOOST_INTRUSIVE_DETAIL_IITERATOR_HPP
#define BOOST_INTRUSIVE_DETAIL_IITERATOR_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/iterator.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>

namespace boost {
namespace intrusive {

template<class ValueTraits>
struct ValueTraitsPointers
{
   typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT
      (boost::intrusive::detail::
      , ValueTraits, ValueTraitsPtr
      , typename boost::intrusive::PointerTraits<typename ValueTraits::node_traits::node_ptr>::template
         rebind_pointer<ValueTraits>::type)   ValueTraitsPtr;

   typedef typename boost::intrusive::PointerTraits<ValueTraitsPtr>::template
      rebind_pointer<ValueTraits const>::type ConstValueTraitsPtr;
};

template<class TValueTraits, bool IsConst, class Category>
struct Iiterator
{
   typedef TValueTraits                                         ValueTraits;
   typedef typename ValueTraits::NodeTraits                  NodeTraits;
   typedef typename NodeTraits::Node                          Node;
   typedef typename NodeTraits::NodePtr                      NodePtr;
   typedef ::boost::intrusive::PointerTraits<NodePtr>        NodepointerTraitsT;
   typedef typename NodepointerTraitsT::template
      rebind_pointer<void>::type                               VoidPointer;
   typedef typename TValueTraits::ValueType                    ValueType;
   typedef typename TValueTraits::pointer                       NonconstPointer;
   typedef typename TValueTraits::const_pointer                 YesconstPointer;
   typedef typename ::boost::intrusive::PointerTraits
      <NonconstPointer>::reference                            NonconstReference;
   typedef typename ::boost::intrusive::PointerTraits
      <YesconstPointer>::reference                            YesconstReference;
   typedef typename NodepointerTraitsT::DifferenceType      DifferenceType;
   typedef typename detail::if_c
      <IsConst, YesconstPointer, NonconstPointer>::type      Pointer;
   typedef typename detail::if_c
      <IsConst, YesconstReference, NonconstReference>::type  Reference;
   typedef Iterator
         < Category
         , ValueType
         , DifferenceType
         , Pointer
         , Reference
         > IteratorType;
   typedef typename ValueTraitsPointers
      <TValueTraits>::ValueTraitsPtr                          ValueTraitsPtr;
   typedef typename ValueTraitsPointers
      <TValueTraits>::ConstValueTraitsPtr                    ConstValueTraitsPtr;
   static const bool STATEFUL_VALUE_TRAITS =
      detail::IsStatefulValueTraits<ValueTraits>::value;
};

template<class NodePtr, class StoredPointer, bool StatefulValueTraits = true>
struct IiteratorMembers
{

   inline IiteratorMembers()
      : m_nodeptr()//Value initialization to achieve "null iterators" (N3644)
   {}

   inline IiteratorMembers(const NodePtr &n_ptr, const StoredPointer &data)
      :  m_nodeptr(n_ptr), m_ptr(data)
   {}

   inline StoredPointer get_ptr() const
   {  return m_ptr;  }

   NodePtr m_nodeptr;
   StoredPointer m_ptr;
};

template<class NodePtr, class StoredPointer>
struct IiteratorMembers<NodePtr, StoredPointer, false>
{
   inline IiteratorMembers()
      : m_nodeptr()//Value initialization to achieve "null iterators" (N3644)
   {}

   inline IiteratorMembers(const NodePtr &n_ptr, const StoredPointer &)
      : m_nodeptr(n_ptr)
   {}

   inline StoredPointer get_ptr() const
   {  return StoredPointer();  }

   NodePtr m_nodeptr;
};

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DETAIL_IITERATOR_HPP
