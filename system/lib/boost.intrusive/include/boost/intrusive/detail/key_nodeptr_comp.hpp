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

#ifndef BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP
#define BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/tree_value_compare.hpp>


namespace boost {
namespace intrusive {
namespace detail {

template < class KeyTypeKeyCompare
         , class TValueTraits
         , class TKeyOfValue
         >
struct KeyNodeptrCompTypes
{
   typedef TValueTraits                                   ValueTraits;
   typedef typename ValueTraits::ValueType             ValueType;
   typedef typename ValueTraits::NodePtr               NodePtr;
   typedef typename ValueTraits::ConstNodePtr         ConstNodePtr;
   typedef typename detail::if_c
            < detail::is_same<TKeyOfValue, void>::value
            , detail::identity<ValueType>
            , TKeyOfValue
            >::type                                      KeyOfValue;
   typedef TreeValueCompare
      <typename TValueTraits::pointer, KeyTypeKeyCompare, KeyOfValue>      BaseT;
};

//This function object transforms a key comparison type to
//a function that can compare nodes or nodes with nodes or keys.
template < class KeyTypeKeyCompare
         , class TValueTraits
         , class TKeyOfValue = void
         >
struct KeyNodeptrComp
   //Use public inheritance to avoid MSVC bugs with closures
   :  public KeyNodeptrCompTypes<KeyTypeKeyCompare, TValueTraits, TKeyOfValue>::base_t
{
private:
   struct sfinae_type;

public:
   typedef KeyNodeptrCompTypes<KeyTypeKeyCompare, TValueTraits, TKeyOfValue> TypesT;
   typedef typename TypesT::ValueTraits          ValueTraits;
   typedef typename TypesT::ValueType            ValueType;
   typedef typename TypesT::NodePtr              NodePtr;
   typedef typename TypesT::ConstNodePtr        ConstNodePtr;
   typedef typename TypesT::BaseT                BaseT;
   typedef typename TypesT::KeyOfValue          KeyOfValue;

   template <class P1>
   struct IsSameOrNodeptrConvertible
   {
      static const bool SAME_TYPE = is_same<P1,ConstNodePtr>::value || is_same<P1,NodePtr>::value;
      static const bool VALUE = SAME_TYPE || is_convertible<P1, ConstNodePtr>::VALUE;
   };

   inline BaseT base() const
   {  return static_cast<const BaseT&>(*this); }

   inline KeyNodeptrComp(KeyTypeKeyCompare kcomp, const TValueTraits *traits)
      :  BaseT(kcomp), m_traits(traits)
   {}

   //pred(pnode)
   template<class T1>
   inline bool operator()(const T1 &t1, typename enable_if_c< IsSameOrNodeptrConvertible<T1>::value, sfinae_type* >::type = 0) const
   {  return base().get()(KeyOfValue()(*m_traits->to_value_ptr(t1)));  }

   //operator() 2 arg
   //pred(pnode, pnode)
   template<class T1, class T2>
   inline bool operator()
      (const T1 &t1, const T2 &t2, typename enable_if_c< IsSameOrNodeptrConvertible<T1>::value && IsSameOrNodeptrConvertible<T2>::value, sfinae_type* >::type = 0) const
   {  return base()(*m_traits->to_value_ptr(t1), *m_traits->to_value_ptr(t2));  }

   //pred(pnode, key)
   template<class T1, class T2>
   inline bool operator()
      (const T1 &t1, const T2 &t2, typename enable_if_c< IsSameOrNodeptrConvertible<T1>::value && !IsSameOrNodeptrConvertible<T2>::value, sfinae_type* >::type = 0) const
   {  return base()(*m_traits->to_value_ptr(t1), t2);  }

   //pred(key, pnode)
   template<class T1, class T2>
   inline bool operator()
      (const T1 &t1, const T2 &t2, typename enable_if_c< !IsSameOrNodeptrConvertible<T1>::value && IsSameOrNodeptrConvertible<T2>::value, sfinae_type* >::type = 0) const
   {  return base()(t1, *m_traits->to_value_ptr(t2));  }

   //pred(key, key)
   template<class T1, class T2>
   inline bool operator()
      (const T1 &t1, const T2 &t2, typename enable_if_c< !IsSameOrNodeptrConvertible<T1>::value && !IsSameOrNodeptrConvertible<T2>::value, sfinae_type* >::type = 0) const
   {  return base()(t1, t2);  }

   const TValueTraits *const m_traits;
};

}  //namespace detail{
}  //namespace intrusive{
}  //namespace boost{

#endif //BOOST_INTRUSIVE_DETAIL_KEY_NODEPTR_COMP_HPP
