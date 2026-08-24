//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2015-2015. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP
#define BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/pointer_traits.hpp>

namespace boost{
namespace intrusive{

//Needed to support smart references to value types
template <class From, class ValuePtr>
struct DisableIfSmartrefTo
   : detail::disable_if_c 
      <  detail::is_same
            <From, typename PointerTraits
               <ValuePtr>
                  ::reference>::value
      || detail::is_same
            <From, typename PointerTraits
                     < typename PointerRebind
                           < ValuePtr
                           , const typename boost::movelib::pointer_element<ValuePtr>::type>::type>
                  ::reference>::value
      >
{};

//This function object takes a KeyCompare function object
//and compares values that contains keys using KeyOfValue
template< class ValuePtr, class TKeyCompare, class TKeyOfValue, class Ret = bool
        , bool = boost::intrusive::detail::is_same
   <typename boost::movelib::pointer_element<ValuePtr>::type, typename TKeyOfValue::type>::value >
struct TreeValueCompare
   :  public boost::intrusive::detail::EboFunctorHolder<TKeyCompare>
{
   typedef typename
      boost::movelib::pointer_element<ValuePtr>::type ValueType;
   typedef TKeyCompare                                 KeyCompare;
   typedef TKeyOfValue                                 KeyOfValue;
   typedef typename TKeyOfValue::type                  KeyType;

   typedef boost::intrusive::detail::EboFunctorHolder<TKeyCompare> BaseT;

   inline TreeValueCompare()
      :  BaseT()
   {}

   inline explicit TreeValueCompare(const KeyCompare &kcomp)
      :  BaseT(kcomp)
   {}

   inline TreeValueCompare (const TreeValueCompare &x)
      :  BaseT(x.BaseT::get())
   {}

   inline TreeValueCompare &operator=(const TreeValueCompare &x)
   {  this->BaseT::get() = x.BaseT::get();   return *this;  }

   inline TreeValueCompare &operator=(const KeyCompare &x)
   {  this->BaseT::get() = x;   return *this;  }

   inline const KeyCompare &key_comp() const
   {  return static_cast<const KeyCompare &>(*this);  }

   inline Ret operator()(const KeyType &key) const
   {  return this->key_comp()(key);   }

   inline Ret operator()(const ValueType &value) const
   {  return this->key_comp()(TKeyOfValue()(value));  }

   template<class U>
   inline Ret operator()( const U &nonkey
                                             , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(nonkey);  }

   inline Ret operator()(const KeyType &key1, const KeyType &key2) const
   {  return this->key_comp()(key1, key2);  }

   inline Ret operator()(const ValueType &value1, const ValueType &value2) const
   {  return this->key_comp()(TKeyOfValue()(value1), TKeyOfValue()(value2));  }

   inline Ret operator()(const KeyType &key1, const ValueType &value2) const
   {  return this->key_comp()(key1, TKeyOfValue()(value2));  }

   inline Ret operator()(const ValueType &value1, const KeyType &key2) const
   {  return this->key_comp()(TKeyOfValue()(value1), key2);  }

   template<class U>
   inline Ret operator()( const KeyType &key1, const U &nonkey2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(key1, nonkey2);  }

   template<class U>
   inline Ret operator()( const U &nonkey1, const KeyType &key2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(nonkey1, key2);  }

   template<class U>
   inline Ret operator()( const ValueType &value1, const U &nonvalue2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(TKeyOfValue()(value1), nonvalue2);  }

   template<class U>
   inline Ret operator()( const U &nonvalue1, const ValueType &value2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(nonvalue1, TKeyOfValue()(value2));  }
};

template<class ValuePtr, class TKeyCompare, class TKeyOfValue, class Ret>
struct TreeValueCompare<ValuePtr, TKeyCompare, TKeyOfValue, Ret, true>
   :  public boost::intrusive::detail::EboFunctorHolder<TKeyCompare>
{
   typedef typename
      boost::movelib::pointer_element<ValuePtr>::type ValueType;
   typedef TKeyCompare                                 KeyCompare;
   typedef TKeyOfValue                                 KeyOfValue;
   typedef typename TKeyOfValue::type                  KeyType;

   typedef boost::intrusive::detail::EboFunctorHolder<TKeyCompare> BaseT;


   inline TreeValueCompare()
      :  BaseT()
   {}

   inline explicit TreeValueCompare(const KeyCompare &kcomp)
      :  BaseT(kcomp)
   {}

   inline TreeValueCompare (const TreeValueCompare &x)
      :  BaseT(x.BaseT::get())
   {}

   inline TreeValueCompare &operator=(const TreeValueCompare &x)
   {  this->BaseT::get() = x.BaseT::get();   return *this;  }

   inline TreeValueCompare &operator=(const KeyCompare &x)
   {  this->BaseT::get() = x;   return *this;  }

   inline const KeyCompare &key_comp() const
   {  return static_cast<const KeyCompare &>(*this);  }

   inline Ret operator()(const KeyType &key) const
   {  return this->key_comp()(key);   }

   template<class U>
   inline Ret operator()( const U &nonkey
                                             , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(nonkey);  }

   inline Ret operator()(const KeyType &key1, const KeyType &key2) const
   {  return this->key_comp()(key1, key2);  }

   template<class U>
   inline Ret operator()( const KeyType &key1, const U &nonkey2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(key1, nonkey2);  }

   template<class U>
   inline Ret operator()(const U &nonkey1, const KeyType &key2
                                              , typename DisableIfSmartrefTo<U, ValuePtr>::type* = 0) const
   {  return this->key_comp()(nonkey1, key2);  }
};

}  //namespace intrusive{
}  //namespace boost{

#endif   //#ifdef BOOST_INTRUSIVE_DETAIL_TREE_VALUE_COMPARE_HPP
