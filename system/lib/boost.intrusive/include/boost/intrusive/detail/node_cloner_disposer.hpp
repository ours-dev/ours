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

#ifndef BOOST_INTRUSIVE_DETAIL_NODE_CLONER_DISPOSER_HPP
#define BOOST_INTRUSIVE_DETAIL_NODE_CLONER_DISPOSER_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/detail/algo_type.hpp>
#include <boost/intrusive/detail/assert.hpp>

namespace boost {
namespace intrusive {
namespace detail {

template<class F, class TValueTraits, AlgoTypes AlgoType, bool IsConst = true>
struct NodeCloner
   //Use public inheritance to avoid MSVC bugs with closures
   :  public EboFunctorHolder<F>
{
   typedef TValueTraits                                      ValueTraits;
   typedef typename ValueTraits::NodeTraits               NodeTraits;
   typedef typename NodeTraits::NodePtr                   NodePtr;
   typedef EboFunctorHolder<F>                            BaseT;
   typedef typename get_algo< AlgoType
                            , NodeTraits>::type            NodeAlgorithms;
   static const bool SAFEMODE_OR_AUTOUNLINK =
      IsSafeAutounlink<ValueTraits::link_mode>::VALUE;
   typedef typename ValueTraits::ValueType                ValueType;
   typedef typename ValueTraits::Pointer                   Pointer;
   typedef typename ValueTraits::ConstPointer             ConstPointer;
   typedef typename NodeTraits::Node                       Node;
   typedef typename ValueTraits::ConstNodePtr            ConstNodePtr;
   typedef typename PointerTraits<Pointer>::Reference      Reference;
   typedef typename PointerTraits
      <ConstPointer>::reference                            ConstReference;
   typedef typename if_c<IsConst, ConstReference, Reference>::type ReferenceType;

   NodeCloner(F f, const TValueTraits *traits)
      :  BaseT(f), m_traits(traits)
   {}

   // tree-based containers use this method, which is proxy-reference friendly
   inline NodePtr operator()(NodePtr p)
   {
      ReferenceType v = *m_traits->to_value_ptr(p);
      NodePtr n = m_traits->to_node_ptr(*BaseT::get()(v));
      //Cloned node must be in default mode if the linking mode requires it
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(n));
      return n;
   }

   const TValueTraits * const m_traits;
};

template<class F, class TValueTraits, AlgoTypes AlgoType>
struct NodeDisposer
   //Use public inheritance to avoid MSVC bugs with closures
   :  public EboFunctorHolder<F>
{
   typedef TValueTraits                          ValueTraits;
   typedef typename ValueTraits::NodeTraits   NodeTraits;
   typedef typename NodeTraits::NodePtr       NodePtr;
   typedef EboFunctorHolder<F>                BaseT;
   typedef typename get_algo< AlgoType
                            , NodeTraits>::type   NodeAlgorithms;
   static const bool SAFEMODE_OR_AUTOUNLINK =
      IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   inline NodeDisposer(F f, const TValueTraits *cont)
      :  BaseT(f), m_traits(cont)
   {}

   inline void operator()(NodePtr p)
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(p);
      BaseT::get()(m_traits->to_value_ptr(p));
   }
   const TValueTraits * const m_traits;
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_NODE_CLONER_DISPOSER_HPP
