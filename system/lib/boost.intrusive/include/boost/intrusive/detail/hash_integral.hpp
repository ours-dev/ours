/////////////////////////////////////////////////////////////////////////////
//
// Copyright 2021-2023 Peter Dimov
// Copyright 2024 Ion Gaztanaga
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
//
// The original C++11 implementation was done by Peter Dimov
// The C++03 porting was done by Ion Gaztanaga
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////


#ifndef BOOST_INTRUSIVE_DETAIL_HASH_INTEGRAL_HPP
#define BOOST_INTRUSIVE_DETAIL_HASH_INTEGRAL_HPP

#include <boost/config.hpp>
#include "hash_mix.hpp"
#include <cstddef>
#include <climits>
#include <boost/intrusive/detail/mpl.hpp>

namespace boost {
namespace intrusive {
namespace detail {


template<class T,
    bool BiggerThanSizeT = (sizeof(T) > sizeof(std::size_t)),
    bool IsUnsigned = is_unsigned<T>::value,
    std::size_t SizeTBits = sizeof(std::size_t) * CHAR_BIT,
    std::size_t TypeBits = sizeof(T) * CHAR_BIT>
struct hash_integral_impl;

template<class T, bool IsUnsigned, std::size_t SizeTBits, std::size_t TypeBits>
struct hash_integral_impl<T, false, IsUnsigned, SizeTBits, TypeBits>
{
    static std::size_t fn( T v )
    {
        return static_cast<std::size_t>( v );
    }
};

template<class T, std::size_t SizeTBits, std::size_t TypeBits>
struct hash_integral_impl<T, true, false, SizeTBits, TypeBits>
{
    static std::size_t fn( T v )
    {
        typedef typename make_unsigned<T>::type U;

        if( v >= 0 )
        {
            return hash_integral_impl<U>::fn( static_cast<U>( v ) );
        }
        else
        {
            return ~hash_integral_impl<U>::fn( static_cast<U>( ~static_cast<U>( v ) ) );
        }
    }
};

template<class T>
struct hash_integral_impl<T, true, true, 32, 64>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 32 ) + (hash_mix)( seed );
        seed = static_cast<std::size_t>( v  & 0xFFFFFFFF ) + (hash_mix)( seed );

        return seed;
    }
};

template<class T>
struct hash_integral_impl<T, true, true, 32, 128>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 96 ) + (hash_mix)( seed );
        seed = static_cast<std::size_t>( v >> 64 ) + (hash_mix)( seed );
        seed = static_cast<std::size_t>( v >> 32 ) + (hash_mix)( seed );
        seed = static_cast<std::size_t>( v ) + (hash_mix)( seed );

        return seed;
    }
};

template<class T>
struct hash_integral_impl<T, true, true, 64, 128>
{
    static std::size_t fn( T v )
    {
        std::size_t seed = 0;

        seed = static_cast<std::size_t>( v >> 64 ) + (hash_mix)( seed );
        seed = static_cast<std::size_t>( v ) + (hash_mix)( seed );

        return seed;
    }
};

template <typename T>
typename enable_if_c<is_integral<T>::value, std::size_t>::type
    hash_value( T v )
{
    return hash_integral_impl<T>::fn( v );
}

} // namespace detail
} // namespace intrusive
} // namespace boost

#endif // #ifndef BOOST_INTRUSIVE_DETAIL_HASH_INTEGRAL_HPP
