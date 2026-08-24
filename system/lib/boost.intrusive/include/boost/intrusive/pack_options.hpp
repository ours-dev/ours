/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2013-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_PACK_OPTIONS_HPP
#define BOOST_INTRUSIVE_PACK_OPTIONS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <cstddef>
#include <utility>

namespace boost {
namespace intrusive {

template<class... Types>
struct Typelist {};

template<std::size_t Index, class List>
struct TypelistElement;

template<std::size_t Index, class Head, class... Tail>
struct TypelistElement<Index, Typelist<Head, Tail...>>
   : TypelistElement<Index - 1, Typelist<Tail...>> {};

template<class Head, class... Tail>
struct TypelistElement<0, Typelist<Head, Tail...>>
{
   using Type = Head;
};

template<class List>
struct InvertTypelist;

template<>
struct InvertTypelist<Typelist<>>
{
   using Type = Typelist<>;
};

template<class... Types>
struct InvertTypelist<Typelist<Types...>>
{
   private:
   template<std::size_t... Indices>
   static auto reverse(std::index_sequence<Indices...>)
      -> Typelist<typename TypelistElement<sizeof...(Types) - Indices - 1, Typelist<Types...>>::Type...>;

   public:
   using Type = decltype(reverse(std::index_sequence_for<Types...>{}));
};

template<class List>
struct DoPack;

template<class Option>
struct DoPack<Typelist<Option>>
{
   using Type = Option;
};

template<class First, class... Remaining>
struct DoPack<Typelist<First, Remaining...>>
{
   using Type = typename First::template Pack<typename DoPack<Typelist<Remaining...>>::Type>;
};

template<class DefaultOptions, class... Options>
struct PackOptions
{
   private:
   using ReversedOptions = typename InvertTypelist<Typelist<DefaultOptions, Options...>>::Type;

   public:
   using Type = typename DoPack<ReversedOptions>::Type;
};

#define BOOST_INTRUSIVE_OPTION_TYPE(OPTION_NAME, TYPE, TYPEDEF_EXPR, TYPEDEF_NAME) \
template<class TypeT> \
struct OPTION_NAME \
{ \
   template<class Base> \
   struct Pack : Base \
   { \
      using TYPEDEF_NAME = TYPEDEF_EXPR; \
   }; \
}; \
//

#define BOOST_INTRUSIVE_OPTION_CONSTANT(OPTION_NAME, TYPE, VALUE, CONSTANT_NAME) \
template<TYPE VALUE> \
struct OPTION_NAME \
{ \
   template<class Base> \
   struct Pack : Base \
   { \
      static constexpr TYPE CONSTANT_NAME = VALUE; \
   }; \
}; \
//

}  // namespace intrusive
}  // namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif  // BOOST_INTRUSIVE_PACK_OPTIONS_HPP
