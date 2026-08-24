/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2022
// (C) Copyright 2022 Joaquin M Lopez Munoz.
// (C) Copyright 2022 Christian Mazakas
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

// fastmod_buckets option is implemented reusing parts of Joaquin M. Lopez
// Munoz's "fxa_unordered" library (proof of concept of closed- and
// open-addressing unordered associative containers), released under
// Boost Software License:
// 
// https://github.com/joaquintides/fxa_unordered/
// 
// On cases and systems that can't take advantage of Daniel Lemire's
// "fastmod" (https://github.com/lemire/fastmod) approach, 
// precomputed divisions are used.
//
// As always, thanks Joaquin for your great work!


#ifndef BOOST_INTRUSIVE_HASHTABLE_HPP
#define BOOST_INTRUSIVE_HASHTABLE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/move/detail/meta_utils_core.hpp>

//General intrusive utilities
#include <boost/intrusive/detail/hashtable_node.hpp>
#include <boost/intrusive/detail/transform_iterator.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>
#include <boost/intrusive/detail/node_to_value.hpp>
#include <boost/intrusive/detail/exception_disposer.hpp>
#include <boost/intrusive/detail/node_cloner_disposer.hpp>
#include <boost/intrusive/detail/simple_disposers.hpp>
#include <boost/intrusive/detail/size_holder.hpp>
#include <boost/intrusive/detail/iterator.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/detail/algorithm.hpp>
#include <boost/intrusive/detail/value_functors.hpp>

//Implementation utilities
#include <boost/intrusive/unordered_set_hook.hpp>
#include <boost/intrusive/detail/slist_iterator.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/circular_slist_algorithms.hpp>
#include <boost/intrusive/linear_slist_algorithms.hpp>

//boost
#include <boost/intrusive/detail/assert.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/move/adl_move_swap.hpp>
#include <boost/move/algo/detail/search.hpp>

//std C++
#include <boost/intrusive/detail/minimal_pair_header.hpp>   //std::pair
#include <cstddef>      //std::size_t
#include <boost/cstdint.hpp>      //std::uint64_t


#include "detail/hash.hpp"

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif


namespace boost {

namespace intrusive {


#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

/// @cond

//We only support LLP64(Win64) or LP64(most Unix) data models
#ifdef _WIN64  //In 64 bit windows sizeof(size_t) == sizeof(unsigned long long)
#  define BOOST_INTRUSIVE_SIZE_C(NUMBER) NUMBER##ULL
#  define BOOST_INTRUSIVE_64_BIT_SIZE_T 1
#else //In 32 bit windows and 32/64 bit unixes sizeof(size_t) == sizeof(unsigned long)
#  define BOOST_INTRUSIVE_SIZE_C(NUMBER) NUMBER##UL
#  define BOOST_INTRUSIVE_64_BIT_SIZE_T (((((ULONG_MAX>>16)>>16)>>16)>>15) != 0)
#endif

template<int Dummy = 0>
struct PrimeListHolder
{
   private:

   template <class SizeType> // sizeof(SizeType) < sizeof(std::size_t)
   static inline SizeType truncate_size_type(std::size_t n, detail::true_)
   {  return n < std::size_t(SizeType(-1)) ? static_cast<SizeType>(n) : SizeType(-1);  }

   template <class SizeType> // sizeof(SizeType) == sizeof(std::size_t)
   static inline SizeType truncate_size_type(std::size_t n, detail::false_)
   {  return static_cast<SizeType>(n);   }

   static const std::size_t PRIME_LIST[];
   static const std::size_t PRIME_LIST_SIZE;

   static const std::size_t *suggested_lower_bucket_count_ptr(std::size_t n)
   {
      const std::size_t *primes     = &PRIME_LIST[0];
      const std::size_t *primes_end = primes + PRIME_LIST_SIZE;
      std::size_t const* bound =
         boost::movelib::lower_bound(primes, primes_end, n, ValueLess<std::size_t>());
      bound -= std::size_t(bound == primes_end);
      return bound;
   }

   static const std::size_t *suggested_upper_bucket_count_ptr(std::size_t n)
   {
      const std::size_t *primes     = &PRIME_LIST[0];
      const std::size_t *primes_end = primes + PRIME_LIST_SIZE;
      std::size_t const* bound =
         boost::movelib::upper_bound(primes, primes_end, n, ValueLess<std::size_t>());
      bound -= std::size_t(bound == primes_end);
      return bound;
   }

   static std::size_t suggested_lower_bucket_count_impl(std::size_t n)
   {  return *suggested_lower_bucket_count_ptr(n); }

   static std::size_t suggested_upper_bucket_count_impl(std::size_t n)
   {  return *suggested_upper_bucket_count_ptr(n); }

   public:

   template <class SizeType>
   static inline SizeType suggested_upper_bucket_count(SizeType n)
   {
      std::size_t const c = suggested_upper_bucket_count_impl(static_cast<std::size_t>(n));
      return truncate_size_type<SizeType>(c, detail::bool_<(sizeof(SizeType) < sizeof(std::size_t))>());
   }

   template <class SizeType>
   static inline SizeType suggested_lower_bucket_count(SizeType n)
   {
      std::size_t const c = suggested_lower_bucket_count_impl(static_cast<std::size_t>(n));
      return truncate_size_type<SizeType>(c, detail::bool_<(sizeof(SizeType) < sizeof(std::size_t))>());
   }

   static inline std::size_t suggested_lower_bucket_count_idx(std::size_t n)
   {  return static_cast<std::size_t>(suggested_lower_bucket_count_ptr(n) - &PRIME_LIST[0]); }

   static inline std::size_t suggested_upper_bucket_count_idx(std::size_t n)
   {  return static_cast<std::size_t>(suggested_upper_bucket_count_ptr(n) - &PRIME_LIST[0]); }

   static inline std::size_t size_from_index(std::size_t n)
   {  return PRIME_LIST[std::ptrdiff_t(n)]; }

   template<std::size_t SizeIndex>
   inline static std::size_t modfunc(std::size_t hash) { return hash % SizeIndex; }

   static std::size_t(*const POSITIONS[])(std::size_t);

   #if BOOST_INTRUSIVE_64_BIT_SIZE_T
   static const uint64_t INV_SIZES32[];
   static const std::size_t INV_SIZES32_SIZE;
   #endif

   inline static std::size_t lower_size_index(std::size_t n)
   {   return PrimeListHolder<>::suggested_lower_bucket_count_idx(n);  }

   inline static std::size_t upper_size_index(std::size_t n)
   {   return PrimeListHolder<>::suggested_upper_bucket_count_idx(n);  }

   inline static std::size_t size(std::size_t size_index)
   {   return PrimeListHolder<>::size_from_index(size_index);  }

   #if BOOST_INTRUSIVE_64_BIT_SIZE_T
   // https://github.com/lemire/fastmod

   inline static uint64_t mul128_u32(uint64_t lowbits, uint32_t d)
   {
      #if defined(_MSC_VER)
         return __umulh(lowbits, d);
      #elif defined(BOOST_HAS_INT128)
         return static_cast<uint64_t>((uint128_type(lowbits) * d) >> 64);
      #else
         uint64_t r1 = (lowbits & UINT32_MAX) * d;
         uint64_t r2 = (lowbits >> 32) * d;
         r2 += r1 >> 32;
         return r2 >> 32;
      #endif
   }

   inline static uint32_t fastmod_u32(uint32_t a, uint64_t m, uint32_t d)
   {
      uint64_t lowbits = m * a;
      return (uint32_t)(mul128_u32(lowbits, d));
   }
   #endif // BOOST_INTRUSIVE_64_BIT_SIZE_T

   inline static std::size_t position(std::size_t hash,std::size_t size_index)
   {
      #if BOOST_INTRUSIVE_64_BIT_SIZE_T
         BOOST_CONSTEXPR_OR_CONST std::size_t SIZES_UNDER_32BIT = sizeof(INV_SIZES32)/sizeof(INV_SIZES32[0]);
         if(BOOST_LIKELY(size_index < SIZES_UNDER_32BIT)){
            return fastmod_u32( uint32_t(hash)+uint32_t(hash>>32)
                              , INV_SIZES32[size_index]
                              , uint32_t(PRIME_LIST[size_index])  );
         }
         else{
            return POSITIONS[size_index](hash);
         }
      #else
         return positions[size_index](hash);
      #endif // BOOST_INTRUSIVE_64_BIT_SIZE_T
  }
};

template<int Dummy>
std::size_t(* const PrimeListHolder<Dummy>::POSITIONS[])(std::size_t) =
{
   modfunc<BOOST_INTRUSIVE_SIZE_C(3)>,                     modfunc<BOOST_INTRUSIVE_SIZE_C(7)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(11)>,                    modfunc<BOOST_INTRUSIVE_SIZE_C(17)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(29)>,                    modfunc<BOOST_INTRUSIVE_SIZE_C(53)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(97)>,                    modfunc<BOOST_INTRUSIVE_SIZE_C(193)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(389)>,                   modfunc<BOOST_INTRUSIVE_SIZE_C(769)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1543)>,                  modfunc<BOOST_INTRUSIVE_SIZE_C(3079)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(6151)>,                  modfunc<BOOST_INTRUSIVE_SIZE_C(12289)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(24593)>,                 modfunc<BOOST_INTRUSIVE_SIZE_C(49157)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(98317)>,                 modfunc<BOOST_INTRUSIVE_SIZE_C(196613)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(393241)>,                modfunc<BOOST_INTRUSIVE_SIZE_C(786433)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1572869)>,               modfunc<BOOST_INTRUSIVE_SIZE_C(3145739)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(6291469)>,               modfunc<BOOST_INTRUSIVE_SIZE_C(12582917)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(25165843)>,              modfunc<BOOST_INTRUSIVE_SIZE_C(50331653)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(100663319)>,             modfunc<BOOST_INTRUSIVE_SIZE_C(201326611)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(402653189)>,             modfunc<BOOST_INTRUSIVE_SIZE_C(805306457)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1610612741)>,            //0-30 indexes
#if BOOST_INTRUSIVE_64_BIT_SIZE_T
   //Taken from Boost.MultiIndex code, thanks to Joaquin M. Lopez Munoz.
   modfunc<BOOST_INTRUSIVE_SIZE_C(3221225473)>,            //<- 32 bit values stop here (index 31)
   modfunc<BOOST_INTRUSIVE_SIZE_C(6442450939)>,            modfunc<BOOST_INTRUSIVE_SIZE_C(12884901893)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(25769803751)>,           modfunc<BOOST_INTRUSIVE_SIZE_C(51539607551)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(103079215111)>,          modfunc<BOOST_INTRUSIVE_SIZE_C(206158430209)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(412316860441)>,          modfunc<BOOST_INTRUSIVE_SIZE_C(824633720831)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1649267441651)>,         modfunc<BOOST_INTRUSIVE_SIZE_C(3298534883309)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(6597069766657)>,         modfunc<BOOST_INTRUSIVE_SIZE_C(13194139533299)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(26388279066623)>,        modfunc<BOOST_INTRUSIVE_SIZE_C(52776558133303)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(105553116266489)>,       modfunc<BOOST_INTRUSIVE_SIZE_C(211106232532969)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(422212465066001)>,       modfunc<BOOST_INTRUSIVE_SIZE_C(844424930131963)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1688849860263953)>,      modfunc<BOOST_INTRUSIVE_SIZE_C(3377699720527861)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(6755399441055731)>,      modfunc<BOOST_INTRUSIVE_SIZE_C(13510798882111483)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(27021597764222939)>,     modfunc<BOOST_INTRUSIVE_SIZE_C(54043195528445957)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(108086391056891903)>,    modfunc<BOOST_INTRUSIVE_SIZE_C(216172782113783843)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(432345564227567621)>,    modfunc<BOOST_INTRUSIVE_SIZE_C(864691128455135207)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(1729382256910270481)>,   modfunc<BOOST_INTRUSIVE_SIZE_C(3458764513820540933)>,
   modfunc<BOOST_INTRUSIVE_SIZE_C(6917529027641081903)>,   modfunc<BOOST_INTRUSIVE_SIZE_C(9223372036854775783)> //(index 63)
#else
   modfunc<BOOST_INTRUSIVE_SIZE_C(2147483647)>             //<- 32 bit stops here (index 31) as ptrdiff_t is signed
#endif
 };

template<int Dummy>
const std::size_t PrimeListHolder<Dummy>::PRIME_LIST[] = {
   BOOST_INTRUSIVE_SIZE_C(3),                     BOOST_INTRUSIVE_SIZE_C(7),
   BOOST_INTRUSIVE_SIZE_C(11),                    BOOST_INTRUSIVE_SIZE_C(17),
   BOOST_INTRUSIVE_SIZE_C(29),                    BOOST_INTRUSIVE_SIZE_C(53),
   BOOST_INTRUSIVE_SIZE_C(97),                    BOOST_INTRUSIVE_SIZE_C(193),
   BOOST_INTRUSIVE_SIZE_C(389),                   BOOST_INTRUSIVE_SIZE_C(769),
   BOOST_INTRUSIVE_SIZE_C(1543),                  BOOST_INTRUSIVE_SIZE_C(3079),
   BOOST_INTRUSIVE_SIZE_C(6151),                  BOOST_INTRUSIVE_SIZE_C(12289),
   BOOST_INTRUSIVE_SIZE_C(24593),                 BOOST_INTRUSIVE_SIZE_C(49157),
   BOOST_INTRUSIVE_SIZE_C(98317),                 BOOST_INTRUSIVE_SIZE_C(196613),
   BOOST_INTRUSIVE_SIZE_C(393241),                BOOST_INTRUSIVE_SIZE_C(786433),
   BOOST_INTRUSIVE_SIZE_C(1572869),               BOOST_INTRUSIVE_SIZE_C(3145739),
   BOOST_INTRUSIVE_SIZE_C(6291469),               BOOST_INTRUSIVE_SIZE_C(12582917),
   BOOST_INTRUSIVE_SIZE_C(25165843),              BOOST_INTRUSIVE_SIZE_C(50331653),
   BOOST_INTRUSIVE_SIZE_C(100663319),             BOOST_INTRUSIVE_SIZE_C(201326611),
   BOOST_INTRUSIVE_SIZE_C(402653189),             BOOST_INTRUSIVE_SIZE_C(805306457),
   BOOST_INTRUSIVE_SIZE_C(1610612741),            //0-30 indexes
#if BOOST_INTRUSIVE_64_BIT_SIZE_T
   //Taken from Boost.MultiIndex code, thanks to Joaquin M. Lopez Munoz.
   BOOST_INTRUSIVE_SIZE_C(3221225473),            //<- 32 bit values stop here (index 31)
   BOOST_INTRUSIVE_SIZE_C(6442450939),            BOOST_INTRUSIVE_SIZE_C(12884901893),
   BOOST_INTRUSIVE_SIZE_C(25769803751),           BOOST_INTRUSIVE_SIZE_C(51539607551),
   BOOST_INTRUSIVE_SIZE_C(103079215111),          BOOST_INTRUSIVE_SIZE_C(206158430209),
   BOOST_INTRUSIVE_SIZE_C(412316860441),          BOOST_INTRUSIVE_SIZE_C(824633720831),
   BOOST_INTRUSIVE_SIZE_C(1649267441651),         BOOST_INTRUSIVE_SIZE_C(3298534883309),
   BOOST_INTRUSIVE_SIZE_C(6597069766657),         BOOST_INTRUSIVE_SIZE_C(13194139533299),
   BOOST_INTRUSIVE_SIZE_C(26388279066623),        BOOST_INTRUSIVE_SIZE_C(52776558133303),
   BOOST_INTRUSIVE_SIZE_C(105553116266489),       BOOST_INTRUSIVE_SIZE_C(211106232532969),
   BOOST_INTRUSIVE_SIZE_C(422212465066001),       BOOST_INTRUSIVE_SIZE_C(844424930131963),
   BOOST_INTRUSIVE_SIZE_C(1688849860263953),      BOOST_INTRUSIVE_SIZE_C(3377699720527861),
   BOOST_INTRUSIVE_SIZE_C(6755399441055731),      BOOST_INTRUSIVE_SIZE_C(13510798882111483),
   BOOST_INTRUSIVE_SIZE_C(27021597764222939),     BOOST_INTRUSIVE_SIZE_C(54043195528445957),
   BOOST_INTRUSIVE_SIZE_C(108086391056891903),    BOOST_INTRUSIVE_SIZE_C(216172782113783843),
   BOOST_INTRUSIVE_SIZE_C(432345564227567621),    BOOST_INTRUSIVE_SIZE_C(864691128455135207),
   BOOST_INTRUSIVE_SIZE_C(1729382256910270481),   BOOST_INTRUSIVE_SIZE_C(3458764513820540933),
   BOOST_INTRUSIVE_SIZE_C(6917529027641081903),   BOOST_INTRUSIVE_SIZE_C(9223372036854775783) //(index 63)
#else
   BOOST_INTRUSIVE_SIZE_C(2147483647)             //<- 32 bit stops here (index 31) as ptrdiff_t is signed
#endif
};

template<int Dummy>
const std::size_t PrimeListHolder<Dummy>::PRIME_LIST_SIZE
   = sizeof(PRIME_LIST) / sizeof(std::size_t);


#if BOOST_INTRUSIVE_64_BIT_SIZE_T

template<int Dummy>
const uint64_t PrimeListHolder<Dummy>::INV_SIZES32[] = {
   BOOST_INTRUSIVE_SIZE_C(6148914691236517206), //3
   BOOST_INTRUSIVE_SIZE_C(2635249153387078803), //7
   BOOST_INTRUSIVE_SIZE_C(1676976733973595602), //11
   BOOST_INTRUSIVE_SIZE_C(1085102592571150096), //17
   BOOST_INTRUSIVE_SIZE_C(636094623231363849),  //29
   BOOST_INTRUSIVE_SIZE_C(348051774975651918),  //53
   BOOST_INTRUSIVE_SIZE_C(190172619316593316),  //97
   BOOST_INTRUSIVE_SIZE_C(95578984837873325),   //193
   BOOST_INTRUSIVE_SIZE_C(47420935922132524),   //389
   BOOST_INTRUSIVE_SIZE_C(23987963684927896),   //769
   BOOST_INTRUSIVE_SIZE_C(11955116055547344),   //1543
   BOOST_INTRUSIVE_SIZE_C(5991147799191151),    //3079
   BOOST_INTRUSIVE_SIZE_C(2998982941588287),    //6151
   BOOST_INTRUSIVE_SIZE_C(1501077717772769),    //12289
   BOOST_INTRUSIVE_SIZE_C(750081082979285),     //24593
   BOOST_INTRUSIVE_SIZE_C(375261795343686),     //49157
   BOOST_INTRUSIVE_SIZE_C(187625172388393),     //98317
   BOOST_INTRUSIVE_SIZE_C(93822606204624),      //196613
   BOOST_INTRUSIVE_SIZE_C(46909513691883),      //393241
   BOOST_INTRUSIVE_SIZE_C(23456218233098),      //786433
   BOOST_INTRUSIVE_SIZE_C(11728086747027),      //1572869
   BOOST_INTRUSIVE_SIZE_C(5864041509391),       //3145739
   BOOST_INTRUSIVE_SIZE_C(2932024948977),       //6291469
   BOOST_INTRUSIVE_SIZE_C(1466014921160),       //12582917
   BOOST_INTRUSIVE_SIZE_C(733007198436),        //25165843
   BOOST_INTRUSIVE_SIZE_C(366503839517),        //50331653
   BOOST_INTRUSIVE_SIZE_C(183251896093),        //100663319
   BOOST_INTRUSIVE_SIZE_C(91625960335),         //201326611
   BOOST_INTRUSIVE_SIZE_C(45812983922),         //402653189
   BOOST_INTRUSIVE_SIZE_C(22906489714),         //805306457
   BOOST_INTRUSIVE_SIZE_C(11453246088),         //1610612741
   BOOST_INTRUSIVE_SIZE_C(5726623060)           //3221225473
};

template<int Dummy>
const std::size_t PrimeListHolder<Dummy>::INV_SIZES32_SIZE
   = sizeof(INV_SIZES32) / sizeof(uint64_t);

#endif // BOOST_INTRUSIVE_64_BIT_SIZE_T

struct PrimeFmodSize : PrimeListHolder<>
{
};


#undef BOOST_INTRUSIVE_SIZE_C
#undef BOOST_INTRUSIVE_64_BIT_SIZE_T

#endif   //#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)



template<class InputIt, class T>
InputIt priv_algo_find(InputIt first, InputIt last, const T& value)
{
   for (; first != last; ++first) {
      if (*first == value) {
         return first;
      }
   }
   return last;
}

template<class InputIt, class T>
typename boost::intrusive::iterator_traits<InputIt>::difference_type
   priv_algo_count(InputIt first, InputIt last, const T& value)
{
   typename boost::intrusive::iterator_traits<InputIt>::difference_type ret = 0;
   for (; first != last; ++first) {
      if (*first == value) {
            ret++;
      }
   }
   return ret;
}

template <class ForwardIterator1, class ForwardIterator2>
bool priv_algo_is_permutation(ForwardIterator1 first1, ForwardIterator1 last1, ForwardIterator2 first2)
{
   typedef typename
      boost::intrusive::iterator_traits<ForwardIterator2>::difference_type
         DistanceType;
   //Efficiently compare identical prefixes: O(N) if sequences
   //have the same elements in the same order.
   for ( ; first1 != last1; ++first1, ++first2){
      if (! (*first1 == *first2))
         break;
   }
   if (first1 == last1){
      return true;
   }

   //Establish last2 assuming equal ranges by iterating over the
   //rest of the list.
   ForwardIterator2 last2 = first2;
   boost::intrusive::iterator_advance(last2, boost::intrusive::iterator_distance(first1, last1));
   for(ForwardIterator1 scan = first1; scan != last1; ++scan){
      if (scan != (priv_algo_find)(first1, scan, *scan)){
         continue;   //We've seen this one before.
      }
      DistanceType matches = (priv_algo_count)(first2, last2, *scan);
      if (0 == matches || (priv_algo_count)(scan, last1, *scan) != matches){
         return false;
      }
   }
   return true;
}

struct HashBoolFlags
{
   static const std::size_t UNIQUE_KEYS_POS        = 1u;
   static const std::size_t CONSTANT_TIME_SIZE_POS = 2u;
   static const std::size_t POWER_2_BUCKETS_POS    = 4u;
   static const std::size_t CACHE_BEGIN_POS        = 8u;
   static const std::size_t COMPARE_HASH_POS       = 16u;
   static const std::size_t INCREMENTAL_POS        = 32u;
   static const std::size_t LINEAR_BUCKETS_POS     = 64u;
   static const std::size_t FASTMOD_BUCKETS_POS    = 128u;
};

template<class Bucket, class Algo, class Disposer, class SizeType>
class ExceptionBucketDisposer
{
   Bucket         *m_cont;
   Disposer       &m_disp;
   const SizeType &m_constructed;

   ExceptionBucketDisposer(const ExceptionBucketDisposer&);
   ExceptionBucketDisposer &operator=(const ExceptionBucketDisposer&);

   public:

   ExceptionBucketDisposer
      (Bucket &cont, Disposer &disp, const SizeType &constructed)
      :  m_cont(&cont), m_disp(disp), m_constructed(constructed)
   {}

   inline void release()
   {  m_cont = 0;  }

   ~ExceptionBucketDisposer()
   {
      SizeType n = m_constructed;
      if(m_cont){
         while(n--){
            Algo::detach_and_dispose(m_cont[n].get_node_ptr(), m_disp);
         }
      }
   }
};

template<class SupposedValueTraits>
struct UnorderedBucketImpl
{
   typedef typename detail::GetNodeTraits
      <SupposedValueTraits>::type               NodeTraits;
   typedef typename ReducedSlistNodeTraits
      <NodeTraits>::type                       ReducedNodeTraits;
   typedef BucketImpl<ReducedNodeTraits>     Type;

   typedef typename PointerTraits
         <typename ReducedNodeTraits::node_ptr>
            ::template rebind_pointer<Type>::type Pointer;
};

template<class SupposedValueTraits>
struct UnorderedBucketPtrImpl
{
   typedef typename UnorderedBucketImpl<SupposedValueTraits>::pointer Type;
};


template <class TBucketPtr, class TSizeType>
struct BucketTraitsImpl
{
private:
   BOOST_COPYABLE_AND_MOVABLE(BucketTraitsImpl)

public:
   /// @cond

   typedef TBucketPtr BucketPtr;
   typedef TSizeType  SizeType;

   /// @endcond

   inline BucketTraitsImpl(BucketPtr buckets, SizeType len)
      : m_buckets(buckets), m_buckets_len(len)
   {}

   inline BucketTraitsImpl(const BucketTraitsImpl& x)
      : m_buckets(x.m_buckets), m_buckets_len(x.m_buckets_len)
   {}

   inline BucketTraitsImpl(BOOST_RV_REF(BucketTraitsImpl) x)
      : m_buckets(x.m_buckets), m_buckets_len(x.m_buckets_len)
   {
      x.m_buckets = BucketPtr();   x.m_buckets_len = 0u;
   }

   inline BucketTraitsImpl& operator=(BOOST_RV_REF(BucketTraitsImpl) x)
   {
      m_buckets = x.m_buckets; m_buckets_len = x.m_buckets_len;
      x.m_buckets = BucketPtr();   x.m_buckets_len = 0u; return *this;
   }

   inline BucketTraitsImpl& operator=(BOOST_COPY_ASSIGN_REF(BucketTraitsImpl) x)
   {
      m_buckets = x.m_buckets;  m_buckets_len = x.m_buckets_len; return *this;
   }

   inline BucketPtr bucket_begin() const
   {
      return m_buckets;
   }

   inline SizeType  bucket_count() const BOOST_NOEXCEPT
   {
      return m_buckets_len;
   }

private:
   BucketPtr  m_buckets;
   SizeType   m_buckets_len;
};


template <class T>
struct StoreHashIsTrue
{
   template<bool Add>
   struct TwoOrThree {detail::yes_type m__[2u + (unsigned)Add];};
   template <class U> static detail::yes_type test(...);
   template <class U> static TwoOrThree<U::store_hash> test (int);
   static const bool VALUE = sizeof(test<T>(0)) > sizeof(detail::yes_type)*2u;
};

template <class T>
struct OptimizeMultikeyIsTrue
{
   template<bool Add>
   struct TwoOrThree { detail::yes_type m__[2u + (unsigned)Add];};
   template <class U> static detail::yes_type test(...);
   template <class U> static TwoOrThree<U::optimize_multikey> test (int);
   static const bool VALUE = sizeof(test<T>(0)) > sizeof(detail::yes_type)*2u;
};

struct InsertCommitDataImpl
{
   std::size_t m_hash;
   std::size_t m_bucket_idx;
   [[nodiscard]] inline std::size_t get_hash() const
   {  return m_hash; }

   inline void set_hash(std::size_t h)
   {  m_hash = h;  }
};

template<class Node, class SlistNodePtr>
inline typename PointerTraits<SlistNodePtr>::template rebind_pointer<Node>::type
   dcast_bucket_ptr(const SlistNodePtr &p)
{
   typedef typename PointerTraits<SlistNodePtr>::template rebind_pointer<Node>::type NodePtr;
   return PointerTraits<NodePtr>::static_cast_from(p);
}

template<class TNodeTraits>
struct GroupFunctions
{
   //           A group is reverse-linked
   //
   //          A is "first in group"
   //          C is "last  in group"
   //           __________________
   //          |  _____   _____   |
   //          | |     | |      | |  <- Group links
   //          ^ V     ^ V      ^ V
   //           _       _        _      _
   //         A|_|    B|_|     C|_|   D|_|
   //
   //          ^ |     ^ |      ^ |    ^ V  <- Bucket links
   //   _ _____| |_____| |______| |____| |
   //  |B|                               |
   //   ^________________________________|
   //

   typedef TNodeTraits                                             NodeTraits;
   typedef UnorderedGroupAdapter<NodeTraits>                   GroupTraits;
   typedef typename NodeTraits::NodePtr                         NodePtr;
   typedef typename NodeTraits::Node                             Node;
   typedef typename ReducedSlistNodeTraits
      <NodeTraits>::type                                         ReducedNodeTraits;
   typedef typename ReducedNodeTraits::node_ptr                 SlistNodePtr;
   typedef typename ReducedNodeTraits::node                     SlistNode;
   typedef CircularSlistAlgorithms<GroupTraits>                GroupAlgorithms;
   typedef CircularSlistAlgorithms<NodeTraits>                 NodeAlgorithms;

   static SlistNodePtr get_bucket_before_begin
      (SlistNodePtr bucket_beg, SlistNodePtr bucket_last, SlistNodePtr sp, detail::true_)
   {
      //First find the last node of p's group.
      //This requires checking the first node of the next group or
      //the bucket node.
      NodePtr p = dcast_bucket_ptr<Node>(sp);
      NodePtr prev_node = p;
      NodePtr nxt(NodeTraits::get_next(p));
      while(!(bucket_beg <= nxt && nxt <= bucket_last) &&
             (GroupTraits::get_next(nxt) == prev_node)){
         prev_node = nxt;
         nxt = NodeTraits::get_next(nxt);
      }

      //If we've reached the bucket node just return it.
      if(bucket_beg <= nxt && nxt <= bucket_last){
         return nxt;
      }

      //Otherwise, iterate using group links until the bucket node
      NodePtr first_node_of_group  = nxt;
      NodePtr last_node_group      = GroupTraits::get_next(first_node_of_group);
      SlistNodePtr possible_end   = NodeTraits::get_next(last_node_group);

      while(!(bucket_beg <= possible_end && possible_end <= bucket_last)){
         first_node_of_group = dcast_bucket_ptr<Node>(possible_end);
         last_node_group   = GroupTraits::get_next(first_node_of_group);
         possible_end      = NodeTraits::get_next(last_node_group);
      }
      return possible_end;
   }

   static SlistNodePtr get_bucket_before_begin
      (SlistNodePtr bucket_beg, SlistNodePtr bucket_last, SlistNodePtr sp, detail::false_)
   {
      //The end node is embedded in the singly linked list:
      //iterate until we reach it.
      while (!(bucket_beg <= sp && sp <= bucket_last)){
         sp = ReducedNodeTraits::get_next(sp);
      }
      return sp;
   }

   static NodePtr get_prev_to_first_in_group(SlistNodePtr bucket_node, NodePtr first_in_group)
   {
      NodePtr nb = dcast_bucket_ptr<Node>(bucket_node);
      NodePtr n;
      while((n = NodeTraits::get_next(nb)) != first_in_group){
         nb = GroupTraits::get_next(n);  //go to last in group
      }
      return nb;
   }

   static void erase_from_group(SlistNodePtr end_ptr, NodePtr to_erase_ptr, detail::true_)
   {
      NodePtr const nxt_ptr(NodeTraits::get_next(to_erase_ptr));
      //Check if the next node is in the group (not end node) and reverse linked to
      //'to_erase_ptr'. Erase if that's the case.
      if(nxt_ptr != end_ptr && to_erase_ptr == GroupTraits::get_next(nxt_ptr)){
         GroupAlgorithms::unlink_after(nxt_ptr);
      }
   }

   inline static void erase_from_group(SlistNodePtr, NodePtr, detail::false_)
   {}

   inline static NodePtr get_last_in_group(NodePtr first_in_group, detail::true_)
   {  return GroupTraits::get_next(first_in_group);  }

   inline static NodePtr get_last_in_group(NodePtr n, detail::false_)
   {  return n;  }

   static NodePtr get_first_in_group(NodePtr n, detail::true_)
   {
      NodePtr ng;
      while(n == NodeTraits::get_next((ng = GroupTraits::get_next(n)))){
         n = ng;
      }
      return n;
   }

   inline static NodePtr get_first_in_group(NodePtr n, detail::false_)
   {  return n;  }

   inline static bool is_first_in_group(NodePtr ptr)
   {  return NodeTraits::get_next(GroupTraits::get_next(ptr)) != ptr;  }


   inline static void insert_in_group(NodePtr first_in_group, NodePtr n, detail::true_)
   {  GroupAlgorithms::link_after(first_in_group, n);  }

   inline static void insert_in_group(NodePtr, NodePtr, detail::false_)
   {}

   //Splits a group in two groups, and makes "new_first" the first node in the second group.
   //Returns the first element of the first group
   static NodePtr split_group(NodePtr const new_first)
   {
      NodePtr const old_first((get_first_in_group)(new_first, detail::true_()));
      //Check new_first was not the first in group
      if(old_first != new_first){
         NodePtr const last = GroupTraits::get_next(old_first);
         GroupTraits::set_next(old_first, GroupTraits::get_next(new_first));
         GroupTraits::set_next(new_first, last);
      }
      return old_first;
   }
};

template<class TBucketType, class TSplitTraits, class SlistNodeAlgorithms>
class IncrementalRehashRollback
{
   private:
   typedef TBucketType   BucketType;
   typedef TSplitTraits  SplitTraits;

   IncrementalRehashRollback();
   IncrementalRehashRollback & operator=(const IncrementalRehashRollback &);
   IncrementalRehashRollback (const IncrementalRehashRollback &);

   public:
   IncrementalRehashRollback
      (BucketType &source_bucket, BucketType &destiny_bucket, SplitTraits &split_tr)
      :  m_source_bucket(source_bucket),  m_destiny_bucket(destiny_bucket)
      ,  m_split_traits(split_tr),  m_released(false)
   {}

   inline void release()
   {  m_released = true; }

   ~IncrementalRehashRollback()
   {
      if(!m_released){
         //If an exception is thrown, just put all moved nodes back in the old bucket
         //and move back the split mark.
         SlistNodeAlgorithms::transfer_after(m_destiny_bucket.get_node_ptr(), m_source_bucket.get_node_ptr());
         m_split_traits.decrement();
      }
   }

   private:
   BucketType &m_source_bucket;
   BucketType &m_destiny_bucket;
   SplitTraits &m_split_traits;
   bool m_released;
};

template<class NodeTraits>
struct NodeFunctions
{
   inline static void store_hash(typename NodeTraits::node_ptr p, std::size_t h, detail::true_)
   {  return NodeTraits::set_hash(p, h); }

   inline static void store_hash(typename NodeTraits::node_ptr, std::size_t, detail::false_)
   {}
};

inline std::size_t hash_to_bucket(std::size_t hash_value, std::size_t bucket_cnt, detail::false_)
{  return hash_value % bucket_cnt;  }

inline std::size_t hash_to_bucket(std::size_t hash_value, std::size_t bucket_cnt, detail::true_)
{  return hash_value & (bucket_cnt - 1);   }

template<bool Power2Buckets, bool Incremental>  //!fastmod_buckets
inline std::size_t hash_to_bucket_split(std::size_t hash_value, std::size_t bucket_cnt, std::size_t split, detail::false_)
{
   std::size_t bucket_number = hash_to_bucket(hash_value, bucket_cnt, detail::bool_<Power2Buckets>());
   BOOST_IF_CONSTEXPR(Incremental)
      bucket_number -= static_cast<std::size_t>(bucket_number >= split)*(bucket_cnt/2);
   return bucket_number;
}

template<bool Power2Buckets, bool Incremental>  //fastmod_buckets
inline std::size_t hash_to_bucket_split(std::size_t hash_value, std::size_t , std::size_t split, detail::true_)
{
   return PrimeFmodSize::position(hash_value, split);
}

//!This metafunction will obtain the type of a bucket
//!from the value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct UnorderedBucket
   : public UnorderedBucketImpl
      < typename ValueTraitsOrHookOption::
         template pack<Empty>::proto_value_traits>
{};

//!This metafunction will obtain the type of a bucket pointer
//!from the value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct UnorderedBucketPtr
   : public UnorderedBucketPtrImpl
      < typename ValueTraitsOrHookOption::
         template pack<Empty>::proto_value_traits>
{};

//!This metafunction will obtain the type of the default bucket traits
//!(when the user does not specify the bucket_traits<> option) from the
//!value_traits or hook option to be used with
//!a hash container.
template<class ValueTraitsOrHookOption>
struct UnorderedDefaultBucketTraits
{
   typedef typename ValueTraitsOrHookOption::
      template pack<Empty>::proto_value_traits     SupposedValueTraits;
   
   typedef BucketTraitsImpl
      < typename UnorderedBucketPtrImpl
            <SupposedValueTraits>::Type
      , std::size_t>                               Type;
};

struct default_bucket_traits;

//hashtable default hook traits
struct DefaultHashtableHookApplier
{  template <class T> struct Apply{ typedef typename T::default_hashtable_hook Type;  };  };

template<>
struct IsDefaultHookTag<DefaultHashtableHookApplier>
{  static const bool VALUE = true;  };

struct HashtableDefaults
{
   typedef DefaultHashtableHookApplier   ProtoValueTraits;
   typedef std::size_t                 SizeType;
   typedef void                        KeyOfValue;
   typedef void                        Equal;
   typedef void                        Hash;
   typedef default_bucket_traits       BucketTraits;
   static const bool CONSTANT_TIME_SIZE   = true;
   static const bool POWER_2_BUCKETS      = false;
   static const bool CACHE_BEGIN          = false;
   static const bool COMPARE_HASH         = false;
   static const bool INCREMENTAL          = false;
   static const bool LINEAR_BUCKETS       = false;
   static const bool FASTMOD_BUCKETS      = false;
};

template<class TValueTraits, bool IsConst>
struct DowncastNodeToValueT
   :  public detail::NodeToValue<TValueTraits, IsConst>
{
   typedef detail::NodeToValue<TValueTraits, IsConst>   BaseT;
   typedef typename BaseT::ResultType                  ResultType;
   typedef TValueTraits                                   ValueTraits;
   typedef typename UnorderedBucketImpl
      <ValueTraits>::type::node_traits::Node            Node;
   typedef typename detail::add_const_if_c
         <Node, IsConst>::type                          &FirstArgumentType;
   typedef typename detail::add_const_if_c
         < typename TValueTraits::node_traits::node
         , IsConst>::type                               &IntermediateArgumentType;
   typedef typename PointerTraits
      <typename TValueTraits::pointer>::
         template rebind_pointer
            <const TValueTraits>::type                   ConstValueTraitsPtr;

   inline DowncastNodeToValueT(ConstValueTraitsPtr ptr)
      :  BaseT(ptr)
   {}

   inline ResultType operator()(FirstArgumentType arg) const
   {  return this->BaseT::operator()(static_cast<IntermediateArgumentType>(arg)); }
};

template<class F, class SlistNodePtr, class NodePtr>
struct NodeCastAdaptor
   //Use public inheritance to avoid MSVC bugs with closures
   :  public detail::EboFunctorHolder<F>
{
   typedef detail::EboFunctorHolder<F> BaseT;

   typedef typename PointerTraits<SlistNodePtr>::element_type SlistNode;
   typedef typename PointerTraits<NodePtr>::element_type      Node;

   template<class ConvertibleToF, class RealValuTraits>
   inline NodeCastAdaptor(const ConvertibleToF &c2f, const RealValuTraits *traits)
      :  BaseT(BaseT(c2f, traits))
   {}

   inline typename BaseT::node_ptr operator()(const SlistNode &to_clone)
   {  return BaseT::operator()(static_cast<const Node &>(to_clone));   }

   inline void operator()(SlistNodePtr to_clone)
   {
      BaseT::operator()(PointerTraits<NodePtr>::pointer_to(static_cast<Node &>(*to_clone)));
   }
};

//bucket_plus_vtraits stores ValueTraits + BucketTraits
//this data is needed by iterators to obtain the
//value from the iterator and detect the bucket
template<class TValueTraits, class TBucketTraits, bool LinearBuckets>
struct BucketPlusVtraits
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BucketPlusVtraits)


   struct DataType
      : public TValueTraits, TBucketTraits
   {
      private:
      BOOST_MOVABLE_BUT_NOT_COPYABLE(DataType)

      public:
      inline DataType(const TValueTraits& val_traits, const TBucketTraits& b_traits)
         : TValueTraits(val_traits), TBucketTraits(b_traits)
      {}

      inline DataType(BOOST_RV_REF(DataType) other)
         : TValueTraits (BOOST_MOVE_BASE(TValueTraits,  other))
         , TBucketTraits(BOOST_MOVE_BASE(TBucketTraits, other))
      {}
   } m_data;

   public:
   typedef TBucketTraits BucketTraits;
   typedef TValueTraits  ValueTraits;

   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   typedef typename UnorderedBucketImpl
      <ValueTraits>::type                               BucketType;
   typedef typename UnorderedBucketPtrImpl
      <ValueTraits>::type                               BucketPtr;
   typedef typename ValueTraits::NodeTraits            NodeTraits;
   typedef typename BucketType::node_traits             SlistNodeTraits;
   typedef UnorderedGroupAdapter<NodeTraits>          GroupTraits;
   typedef GroupFunctions<NodeTraits>                  GroupFunctionsT;
   typedef typename detail::if_c
      < LinearBuckets
      , LinearSlistAlgorithms<SlistNodeTraits>
      , CircularSlistAlgorithms<SlistNodeTraits>
      >::type                                            SlistNodeAlgorithms;

   typedef typename SlistNodeTraits::node_ptr          SlistNodePtr;
   typedef TrivialValueTraits
      <SlistNodeTraits, NormalLink>                   SlistValueTraits;
   typedef SlistIterator<SlistValueTraits, false>     Siterator;
   typedef SlistIterator<SlistValueTraits, true>      ConstSiterator;

   typedef typename NodeTraits::NodePtr                NodePtr;
   typedef typename NodeTraits::ConstNodePtr          ConstNodePtr;
   typedef typename NodeTraits::Node                    Node;
   typedef typename ValueTraits::ValueType             ValueType;
   typedef typename ValueTraits::Pointer                Pointer;
   typedef typename ValueTraits::ConstPointer          ConstPointer;
   typedef typename PointerTraits<Pointer>::Reference   Reference;
   typedef typename PointerTraits
      <ConstPointer>::reference                         ConstReference;
   typedef CircularSlistAlgorithms<GroupTraits>       GroupAlgorithms;
   typedef typename PointerTraits
      <typename ValueTraits::pointer>::
         template rebind_pointer
            <const ValueTraits>::type                   ConstValueTraitsPtr;
   typedef typename PointerTraits
      <typename ValueTraits::pointer>::
         template rebind_pointer
            <const BucketPlusVtraits>::type            ConstBucketValueTraitsPtr;
   typedef detail::bool_<LinearBuckets>                  LinearBucketsT;
   typedef BucketPlusVtraits&                          ThisRef;

   static const std::size_t BUCKET_OVERHEAD = LinearBuckets ? 1u : 0u;

   inline BucketPlusVtraits(const TValueTraits &val_traits, const BucketTraits &b_traits)
      : m_data(val_traits, b_traits)
   {}

   inline BucketPlusVtraits(BOOST_RV_REF(BucketPlusVtraits) other)
      : m_data(boost::move(((BucketPlusVtraits&)other).m_data))
   {}

   inline ConstValueTraitsPtr priv_value_traits_ptr() const
   {  return PointerTraits<ConstValueTraitsPtr>::pointer_to(this->priv_value_traits());  }

   //bucket_value_traits
   //
   inline const BucketPlusVtraits &get_bucket_value_traits() const
   {  return *this;  }

   inline BucketPlusVtraits &get_bucket_value_traits()
   {  return *this;  }

   inline ConstBucketValueTraitsPtr bucket_value_traits_ptr() const
   {  return PointerTraits<ConstBucketValueTraitsPtr>::pointer_to(this->get_bucket_value_traits());  }

   //value traits
   //
   inline const ValueTraits &priv_value_traits() const
   {  return static_cast<const ValueTraits &>(this->m_data);  }

   inline ValueTraits &priv_value_traits()
   {  return static_cast<ValueTraits &>(this->m_data);  }

   //value traits
   //
   inline const BucketTraits &priv_bucket_traits() const
   {  return static_cast<const BucketTraits &>(this->m_data);  }

   inline BucketTraits& priv_bucket_traits()
   {  return static_cast<BucketTraits&>(this->m_data);  }

   //bucket operations
   inline BucketPtr priv_bucket_pointer() const BOOST_NOEXCEPT
   {  return this->priv_bucket_traits().bucket_begin();  }

   [[nodiscard]] inline std::size_t priv_usable_bucket_count() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(BUCKET_OVERHEAD){
         const std::size_t n = this->priv_bucket_traits().bucket_count();
         return n - std::size_t(n != 0)*BUCKET_OVERHEAD;
      }
      else{
         return this->priv_bucket_traits().bucket_count();
      }
   }

   inline BucketType &priv_bucket(std::size_t n) const BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(n < this->priv_usable_bucket_count());
      return this->priv_bucket_pointer()[std::ptrdiff_t(n)];
   }

   inline BucketPtr priv_bucket_ptr(std::size_t n) const BOOST_NOEXCEPT
   {  return PointerTraits<BucketPtr>::pointer_to(this->priv_bucket(n)); }

   inline BucketPtr priv_past_usable_bucket_ptr() const
   {  return this->priv_bucket_pointer() + std::ptrdiff_t(priv_usable_bucket_count()); }

   inline BucketPtr priv_invalid_bucket_ptr() const
   {
      BOOST_IF_CONSTEXPR(LinearBuckets) {
         return BucketPtr();
      }
      else{
         return this->priv_past_usable_bucket_ptr();
      }
   }

   inline void priv_set_sentinel_bucket() const
   {
      BOOST_IF_CONSTEXPR(LinearBuckets) {
         BOOST_INTRUSIVE_INVARIANT_ASSERT(this->priv_bucket_traits().bucket_count() > 1);
         BucketType &b = this->priv_bucket_pointer()[std::ptrdiff_t(this->priv_usable_bucket_count())];
         SlistNodeAlgorithms::set_sentinel(b.get_node_ptr());
      }
   }

   inline void priv_unset_sentinel_bucket() const
   {
      BOOST_IF_CONSTEXPR(LinearBuckets) {
         BOOST_INTRUSIVE_INVARIANT_ASSERT(this->priv_bucket_traits().bucket_count() > 1);
         BucketType& b = this->priv_bucket_pointer()[std::ptrdiff_t(this->priv_usable_bucket_count())];
         SlistNodeAlgorithms::init_header(b.get_node_ptr());
      }
   }

   inline Siterator priv_end_sit() const
   {  return priv_end_sit(LinearBucketsT());  }

   inline Siterator priv_end_sit(detail::true_) const
   {  return Siterator(this->priv_bucket_pointer() + std::ptrdiff_t(this->priv_bucket_traits().bucket_count() - BUCKET_OVERHEAD)); }

   inline Siterator priv_end_sit(detail::false_) const
   {  return Siterator(this->priv_bucket_pointer()->get_node_ptr());  }

   inline Siterator priv_bucket_lbegin(std::size_t n) const
   {  Siterator s(this->priv_bucket_lbbegin(n)); return ++s; }

   inline Siterator priv_bucket_lbbegin(std::size_t n) const
   {  return this->sit_bbegin(this->priv_bucket(n));  }

   inline Siterator priv_bucket_lend(std::size_t n) const
   {  return this->sit_end(this->priv_bucket(n));  }

   [[nodiscard]] inline std::size_t priv_bucket_size(std::size_t n) const
   {  return SlistNodeAlgorithms::count(this->priv_bucket(n).get_node_ptr())-1u;  }

   [[nodiscard]] inline bool priv_bucket_empty(std::size_t n) const
   {  return SlistNodeAlgorithms::is_empty(this->priv_bucket(n).get_node_ptr());  }

   inline bool priv_bucket_empty(BucketPtr p) const
   {  return SlistNodeAlgorithms::is_empty(p->get_node_ptr());  }

   static inline Siterator priv_bucket_lbegin(BucketType &b)
   {  return Siterator(SlistNodeTraits::get_next(b.get_node_ptr()));  }

   static inline Siterator priv_bucket_lbbegin(BucketType& b)
   {  return Siterator(b.get_node_ptr());  }

   static inline Siterator priv_bucket_lend(BucketType& b)
   {  return Siterator(SlistNodeAlgorithms::end_node(b.get_node_ptr()));  }

   static inline std::size_t priv_bucket_size(const BucketType& b)
   {  return SlistNodeAlgorithms::count(b.get_node_ptr())-1u;  }

   static inline bool priv_bucket_empty(const BucketType& b)
   {  return SlistNodeAlgorithms::is_empty(b.get_node_ptr());  }

   template<class NodeDisposer>
   static std::size_t priv_erase_from_single_bucket
      (BucketType &b, Siterator sbefore_first, Siterator slast, NodeDisposer node_disposer, detail::true_)   //optimize multikey
   {
      std::size_t n = 0;
      Siterator const sfirst(++Siterator(sbefore_first));
      if(sfirst != slast){
         NodePtr const nf = dcast_bucket_ptr<Node>(sfirst.pointed_node());
         NodePtr const nl = dcast_bucket_ptr<Node>(slast.pointed_node());
         SlistNodePtr const ne = (priv_bucket_lend(b)).pointed_node();

         if(GroupFunctionsT::is_first_in_group(nf)) {
            // The first node is at the beginning of a group.
            if(nl != ne){
               GroupFunctionsT::split_group(nl);
            }
         }
         else {
            NodePtr const group1 = GroupFunctionsT::split_group(nf);
            if(nl != ne) {
               NodePtr const group2 = GroupFunctionsT::split_group(nl);
               if(nf == group2) {   //Both first and last in the same group
                                    //so join group1 and group2
                  NodePtr const end1 = GroupTraits::get_next(group1);
                  NodePtr const end2 = GroupTraits::get_next(group2);
                  GroupTraits::set_next(group1, end2);
                  GroupTraits::set_next(nl, end1);
               }
            }
         }

         n = SlistNodeAlgorithms::unlink_after_and_dispose(sbefore_first.pointed_node(), slast.pointed_node(), node_disposer);
      }
      return n;
   }

   template<class NodeDisposer>
   static std::size_t priv_erase_from_single_bucket
      (BucketType &, Siterator sbefore_first, Siterator slast, NodeDisposer node_disposer, detail::false_)   //optimize multikey
   {
      return SlistNodeAlgorithms::unlink_after_and_dispose(sbefore_first.pointed_node(), slast.pointed_node(), node_disposer);
   }

   template<class NodeDisposer>
   static void priv_erase_node(BucketType &b, Siterator i, NodeDisposer node_disposer, detail::true_)   //optimize multikey
   {
      SlistNodePtr const ne(priv_bucket_lend(b).pointed_node());
      SlistNodePtr const nbb(priv_bucket_lbbegin(b).pointed_node());
      NodePtr n(dcast_bucket_ptr<Node>(i.pointed_node()));
      NodePtr pos = NodeTraits::get_next(GroupTraits::get_next(n));
      NodePtr bn;
      NodePtr nn(NodeTraits::get_next(n));
      
      if(pos != n) {
         //Node is the first of the group
         bn = GroupFunctionsT::get_prev_to_first_in_group(nbb, n);

         //Unlink the rest of the group if it's not the last node of its group
         if(nn != ne && GroupTraits::get_next(nn) == n){
            GroupAlgorithms::unlink_after(nn);
         }
      }
      else if(nn != ne && GroupTraits::get_next(nn) == n){
         //Node is not the end of the group
         bn = GroupTraits::get_next(n);
         GroupAlgorithms::unlink_after(nn);
      }
      else{
         //Node is the end of the group
         bn = GroupTraits::get_next(n);
         NodePtr const x(GroupAlgorithms::get_previous_node(n));
         GroupAlgorithms::unlink_after(x);
      }
      SlistNodeAlgorithms::unlink_after_and_dispose(bn, node_disposer);
   }

   template<class NodeDisposer>
   inline static void priv_erase_node(BucketType &b, Siterator i, NodeDisposer node_disposer, detail::false_)   //!optimize multikey
   {
      SlistNodePtr bi = SlistNodeAlgorithms::get_previous_node(b.get_node_ptr(), i.pointed_node());
      SlistNodeAlgorithms::unlink_after_and_dispose(bi, node_disposer);
   }

   template<class NodeDisposer, bool OptimizeMultikey>
   std::size_t priv_erase_node_range( Siterator const &before_first_it,  std::size_t const first_bucket
                        , Siterator const &last_it,          std::size_t const last_bucket
                        , NodeDisposer node_disposer, detail::bool_<OptimizeMultikey> optimize_multikey_tag)
   {
      std::size_t num_erased(0);
      Siterator last_step_before_it;
      if(first_bucket != last_bucket){
         BucketType *b = &this->priv_bucket(0);
         num_erased += this->priv_erase_from_single_bucket
            (b[first_bucket], before_first_it, this->priv_bucket_lend(first_bucket), node_disposer, optimize_multikey_tag);
         for(std::size_t i = 0, n = (last_bucket - first_bucket - 1); i != n; ++i){
            num_erased += this->priv_erase_whole_bucket(b[first_bucket+i+1], node_disposer);
         }
         last_step_before_it = this->priv_bucket_lbbegin(last_bucket);
      }
      else{
         last_step_before_it = before_first_it;
      }
      num_erased += this->priv_erase_from_single_bucket
                  (this->priv_bucket(last_bucket), last_step_before_it, last_it, node_disposer, optimize_multikey_tag);
      return num_erased;
   }

   static Siterator priv_get_last(BucketType &b, detail::true_)  //optimize multikey
   {
      //First find the last node of p's group.
      //This requires checking the first node of the next group or
      //the bucket node.
      SlistNodePtr end_ptr(sit_end(b).pointed_node());
      SlistNodePtr last_node_group(b.get_node_ptr());
      SlistNodePtr possible_end(SlistNodeTraits::get_next(last_node_group));

      while(end_ptr != possible_end){
         last_node_group   = GroupTraits::get_next(dcast_bucket_ptr<Node>(possible_end));
         possible_end      = SlistNodeTraits::get_next(last_node_group);
      }
      return Siterator(last_node_group);
   }

   inline static Siterator priv_get_last(BucketType &b, detail::false_) //NOT optimize multikey
   {
      SlistNodePtr p = b.get_node_ptr();
      return Siterator(SlistNodeAlgorithms::get_previous_node(p, SlistNodeAlgorithms::end_node(p)));
   }

   template<class NodeDisposer>
   static inline std::size_t priv_erase_whole_bucket(BucketType &b, NodeDisposer node_disposer)
   {  return SlistNodeAlgorithms::detach_and_dispose(b.get_node_ptr(), node_disposer);  }

   static Siterator priv_get_previous(BucketType &b, Siterator i, detail::true_)   //optimize multikey
   {
      NodePtr const elem(dcast_bucket_ptr<Node>(i.pointed_node()));
      NodePtr const prev_in_group(GroupTraits::get_next(elem));
      bool const first_in_group = NodeTraits::get_next(prev_in_group) != elem;
      SlistNodePtr n = first_in_group
         ? GroupFunctionsT::get_prev_to_first_in_group(b.get_node_ptr(), elem)
         : GroupTraits::get_next(elem)
         ;
      return Siterator(n);
   }

   inline static Siterator priv_get_previous(BucketType &b, Siterator i, detail::false_)   //NOT optimize multikey
   {  return Siterator(SlistNodeAlgorithms::get_previous_node(b.get_node_ptr(), i.pointed_node()));   }

   template<class Disposer>
   struct TypeofNodeDisposer
   {
      typedef NodeCastAdaptor
         < detail::NodeDisposer< Disposer, ValueTraits, CommonSListAlgorithmsValue>
         , SlistNodePtr, NodePtr > Type;
   };

   template<class Disposer>
   inline typename TypeofNodeDisposer<Disposer>::type
      make_node_disposer(const Disposer &disposer) const
   {
      typedef typename TypeofNodeDisposer<Disposer>::type ReturnT;
      return ReturnT(disposer, &this->priv_value_traits());
   }

   static inline BucketPtr to_ptr(BucketType &b)
   {  return PointerTraits<BucketPtr>::pointer_to(b);   }

   static inline Siterator sit_bbegin(BucketType& b)
   {  return Siterator(b.get_node_ptr());  }

   static inline Siterator sit_begin(BucketType& b)
   {  return Siterator(b.begin_ptr());  }

   static inline Siterator sit_end(BucketType& b)
   {  return Siterator(SlistNodeAlgorithms::end_node(b.get_node_ptr()));  }

   inline static std::size_t priv_stored_hash(Siterator s, detail::true_) //store_hash
   {  return NodeTraits::get_hash(dcast_bucket_ptr<Node>(s.pointed_node()));  }

   inline static std::size_t priv_stored_hash(Siterator, detail::false_)  //NO store_hash
   {  return std::size_t(-1);   }

   inline Node &priv_value_to_node(Reference v)
   {  return *this->priv_value_traits().to_node_ptr(v);  }

   inline const Node &priv_value_to_node(ConstReference v) const
   {  return *this->priv_value_traits().to_node_ptr(v);  }

   inline NodePtr priv_value_to_node_ptr(Reference v)
   {  return this->priv_value_traits().to_node_ptr(v);  }

   inline ConstNodePtr priv_value_to_node_ptr(ConstReference v) const
   {  return this->priv_value_traits().to_node_ptr(v);  }

   inline Reference priv_value_from_siterator(Siterator s)
   {  return *this->priv_value_traits().to_value_ptr(dcast_bucket_ptr<Node>(s.pointed_node())); }

   inline ConstReference priv_value_from_siterator(Siterator s) const
   {  return *this->priv_value_traits().to_value_ptr(dcast_bucket_ptr<Node>(s.pointed_node())); }

   static void priv_init_buckets(const BucketPtr buckets_ptr, const std::size_t bucket_cnt)
   {
      BucketPtr buckets_it = buckets_ptr;
      for (std::size_t bucket_i = 0; bucket_i != bucket_cnt; ++buckets_it, ++bucket_i) {
         SlistNodeAlgorithms::init_header(buckets_it->get_node_ptr());
      }
   }

   void priv_clear_buckets(const BucketPtr buckets_ptr, const std::size_t bucket_cnt)
   {
      BucketPtr buckets_it = buckets_ptr;
      for(std::size_t bucket_i = 0; bucket_i != bucket_cnt; ++buckets_it, ++bucket_i){
         BucketType &b = *buckets_it;
         BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
            SlistNodeAlgorithms::detach_and_dispose(b.get_node_ptr(), this->make_node_disposer(detail::NullDisposer()));
         }
         else{
            SlistNodeAlgorithms::init_header(b.get_node_ptr());
         }
      }
   }

   inline std::size_t priv_stored_or_compute_hash(const ValueType &v, detail::true_) const   //For store_hash == true
   {  return NodeTraits::get_hash(this->priv_value_traits().to_node_ptr(v));  }

   typedef HashtableIterator<BucketPlusVtraits, LinearBuckets, false>          Iterator;
   typedef HashtableIterator<BucketPlusVtraits, LinearBuckets, true>           ConstIterator;

   inline Iterator end() BOOST_NOEXCEPT
   {  return this->build_iterator(this->priv_end_sit(), BucketPtr());   }

   inline ConstIterator end() const BOOST_NOEXCEPT
   {  return this->cend();   }

   inline ConstIterator cend() const BOOST_NOEXCEPT
   {  return this->build_const_iterator(this->priv_end_sit(), BucketPtr());   }

   inline Iterator build_iterator(Siterator s, BucketPtr p)
   {  return this->build_iterator(s, p, LinearBucketsT());  }

   inline Iterator build_iterator(Siterator s, BucketPtr p, detail::true_)   //linear buckets
   {  return Iterator(s, p, this->priv_value_traits_ptr());  }

   inline Iterator build_iterator(Siterator s, BucketPtr, detail::false_)    //!linear buckets
   {  return Iterator(s, &this->get_bucket_value_traits());  }

   inline ConstIterator build_const_iterator(Siterator s, BucketPtr p) const
   {  return this->build_const_iterator(s, p, LinearBucketsT());  }

   inline ConstIterator build_const_iterator(Siterator s, BucketPtr p, detail::true_) const   //linear buckets
   {  return ConstIterator(s, p, this->priv_value_traits_ptr());  }

   inline ConstIterator build_const_iterator(Siterator s, BucketPtr, detail::false_) const   //!linear buckets
   {  return ConstIterator(s, &this->get_bucket_value_traits());  }
};

template<class Hash, class>
struct GetHash
{
   typedef Hash Type;
};

template<class T>
struct GetHash<void, T>
{
   typedef detail::InternalHashFunctor<T> Type;
};

template<class EqualTo, class>
struct GetEqualTo
{
   typedef EqualTo Type;
};

template<class T>
struct GetEqualTo<void, T>
{
   typedef ValueEqual<T> Type;
};

template<class KeyOfValue, class T>
struct GetHashKeyOfValue
{
   typedef KeyOfValue Type;
};

template<class T>
struct GetHashKeyOfValue<void, T>
{
   typedef ::boost::intrusive::detail::identity<T> Type;
};

template<class T, class VoidOrKeyOfValue>
struct HashKeyTypesBase
{
   typedef typename GetHashKeyOfValue
      < VoidOrKeyOfValue, T>::type           KeyOfValue;
   typedef typename KeyOfValue::type   KeyType;
};

template<class T, class VoidOrKeyOfValue, class VoidOrKeyHash>
struct HashKeyHash
   : GetHash
      < VoidOrKeyHash
      , typename HashKeyTypesBase<T, VoidOrKeyOfValue>::key_type
      >
{};

template<class T, class VoidOrKeyOfValue, class VoidOrKeyEqual>
struct HashKeyEqual
   : GetEqualTo
      < VoidOrKeyEqual
      , typename HashKeyTypesBase<T, VoidOrKeyOfValue>::key_type
      >

{};

//bucket_hash_t
//Stores bucket_plus_vtraits plust the hash function
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class TBucketTraits, bool LinearBuckets>
struct BucketHashT
   //Use public inheritance to avoid MSVC bugs with closures
   : public detail::EboFunctorHolder
      <typename HashKeyHash < typename BucketPlusVtraits<TValueTraits,TBucketTraits, LinearBuckets >::value_traits::value_type
                              , VoidOrKeyOfValue
                              , VoidOrKeyHash
                              >::type
      >
   , BucketPlusVtraits<TValueTraits, TBucketTraits, LinearBuckets>  //4
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BucketHashT)

   public:

   typedef typename BucketPlusVtraits
      <TValueTraits,TBucketTraits, LinearBuckets>::ValueTraits                       ValueTraits;
   typedef typename ValueTraits::ValueType                                        ValueType;
   typedef typename ValueTraits::NodeTraits                                       NodeTraits;
   typedef HashKeyHash
      < ValueType, VoidOrKeyOfValue, VoidOrKeyHash>                                HashKeyHashT;
   typedef typename HashKeyHashT::type                                           Hasher;
   typedef typename HashKeyTypesBase<ValueType, VoidOrKeyOfValue>::KeyOfValue KeyOfValue;

   typedef TBucketTraits BucketTraits;
   typedef BucketPlusVtraits<TValueTraits, TBucketTraits, LinearBuckets> BucketPlusVtraitsT;
   typedef detail::EboFunctorHolder<Hasher> BaseT;

   inline BucketHashT(const TValueTraits &val_traits, const BucketTraits &b_traits, const Hasher & h)
      : BaseT(h)
      , BucketPlusVtraitsT(val_traits, b_traits)
   {}

   inline BucketHashT(BOOST_RV_REF(BucketHashT) other)
      : BaseT(BOOST_MOVE_BASE(BaseT, other))
      , BucketPlusVtraitsT(BOOST_MOVE_BASE(BucketPlusVtraitsT, other))
   {}

   template<class K>
   inline std::size_t priv_hash(const K &k) const
   {  return this->BaseT::operator()(k);  }

   inline const Hasher &priv_hasher() const
   {  return this->BaseT::get();  }

   inline Hasher &priv_hasher()
   {  return this->BaseT::get();  }

   using BucketPlusVtraitsT::priv_stored_or_compute_hash;   //For store_hash == true

   inline std::size_t priv_stored_or_compute_hash(const ValueType &v, detail::false_) const  //For store_hash == false
   {  return this->priv_hasher()(KeyOfValue()(v));   }
};

template<class ValueTraits, class BucketTraits, class VoidOrKeyOfValue, class VoidOrKeyEqual, bool LinearBuckets>
struct HashtableEqualHolder
{
   typedef detail::EboFunctorHolder
      < typename HashKeyEqual  < typename BucketPlusVtraits
                                       <ValueTraits, BucketTraits, LinearBuckets>::value_traits::value_type
                                 , VoidOrKeyOfValue
                                 , VoidOrKeyEqual
                                 >::Type
      > Type;
};


//bucket_hash_equal_t
//Stores bucket_hash_t and the equality function when the first
//non-empty bucket shall not be cached.
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TBucketTraits, bool LinearBuckets, bool>
struct BucketHashEqualT
   //Use public inheritance to avoid MSVC bugs with closures
   : public BucketHashT<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, TBucketTraits, LinearBuckets> //3
   , public HashtableEqualHolder<TValueTraits, TBucketTraits, VoidOrKeyOfValue, VoidOrKeyEqual, LinearBuckets>::type //equal
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BucketHashEqualT)

   public:
   typedef typename HashtableEqualHolder
      < TValueTraits, TBucketTraits, VoidOrKeyOfValue
      , VoidOrKeyEqual, LinearBuckets>::type                EqualHolderT;
   typedef BucketHashT< TValueTraits, VoidOrKeyOfValue
                        , VoidOrKeyHash, TBucketTraits
                        , LinearBuckets>                    BucketHashType;
   typedef BucketPlusVtraits
      <TValueTraits, TBucketTraits, LinearBuckets>            BucketPlusVtraitsT;
   typedef TValueTraits                                      ValueTraits;
   typedef typename EqualHolderT::functor_type            KeyEqual;
   typedef typename BucketHashType::Hasher                Hasher;
   typedef TBucketTraits                                     BucketTraits;
   typedef typename BucketPlusVtraitsT::Siterator        Siterator;
   typedef typename BucketPlusVtraitsT::ConstSiterator  ConstSiterator;
   typedef typename BucketPlusVtraitsT::BucketType      BucketType;
   typedef typename BucketPlusVtraitsT::SlistNodeAlgorithms  SlistNodeAlgorithms;
   typedef typename UnorderedBucketPtrImpl
      <ValueTraits>::type                                  BucketPtr;

   BucketHashEqualT(const TValueTraits &val_traits, const BucketTraits &b_traits, const Hasher & h, const KeyEqual &e)
      : BucketHashType(val_traits, b_traits, h)
      , EqualHolderT(e)
   {}

   inline BucketHashEqualT(BOOST_RV_REF(BucketHashEqualT) other)
      : BucketHashType(BOOST_MOVE_BASE(BucketHashType, other))
      , EqualHolderT(BOOST_MOVE_BASE(EqualHolderT, other))
   {}

   inline BucketPtr priv_get_cache()
   {  return this->priv_bucket_pointer();   }

   inline void priv_set_cache(BucketPtr)
   {}

   inline void priv_set_cache_bucket_num(std::size_t)
   {}

   inline std::size_t priv_get_cache_bucket_num()
   {  return 0u;  }

   inline void priv_init_cache()
   {}

   inline void priv_swap_cache(BucketHashEqualT &)
   {}

   Siterator priv_begin(BucketPtr &pbucketptr) const
   {
      std::size_t n = 0;
      std::size_t bucket_cnt = this->priv_usable_bucket_count();
      for (n = 0; n < bucket_cnt; ++n){
         BucketType &b = this->priv_bucket(n);
         if(!SlistNodeAlgorithms::is_empty(b.get_node_ptr())){
            pbucketptr = this->to_ptr(b);
            return Siterator(b.begin_ptr());
         }
      }
      pbucketptr = this->priv_invalid_bucket_ptr();
      return this->priv_end_sit();
   }

   inline void priv_insertion_update_cache(std::size_t)
   {}

   inline void priv_erasure_update_cache_range(std::size_t, std::size_t)
   {}

   inline void priv_erasure_update_cache(BucketPtr)
   {}

   inline void priv_erasure_update_cache()
   {}

   inline const KeyEqual &priv_equal() const
   {  return this->EqualHolderT::get();  }

   inline KeyEqual &priv_equal()
   {  return this->EqualHolderT::get();  }
};

//bucket_hash_equal_t
//Stores bucket_hash_t and the equality function when the first
//non-empty bucket shall be cached.
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TBucketTraits, bool LinearBuckets>  //cache_begin == true version
struct BucketHashEqualT<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, LinearBuckets, true>
   //Use public inheritance to avoid MSVC bugs with closures
   : public BucketHashT<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, TBucketTraits, LinearBuckets> //2
   , public HashtableEqualHolder<TValueTraits, TBucketTraits, VoidOrKeyOfValue, VoidOrKeyEqual, LinearBuckets>::type
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BucketHashEqualT)

   public:

   typedef typename HashtableEqualHolder
      < TValueTraits, TBucketTraits
      , VoidOrKeyOfValue, VoidOrKeyEqual, LinearBuckets>::type       EqualHolderT;

   typedef BucketPlusVtraits
      < TValueTraits, TBucketTraits, LinearBuckets>                    BucketPlusVtraitsT;
   typedef TValueTraits                                               ValueTraits;
   typedef typename EqualHolderT::functor_type                     KeyEqual;
   typedef BucketHashT
      < TValueTraits, VoidOrKeyOfValue
      , VoidOrKeyHash, TBucketTraits, LinearBuckets>                  BucketHashType;
   typedef typename BucketHashType::Hasher                         Hasher;
   typedef TBucketTraits                                              BucketTraits;
   typedef typename BucketPlusVtraitsT::Siterator                 Siterator;
   typedef typename BucketPlusVtraitsT::SlistNodeAlgorithms     SlistNodeAlgorithms;

   BucketHashEqualT(const TValueTraits &val_traits, const BucketTraits &b_traits, const Hasher & h, const KeyEqual &e)
      : BucketHashType(val_traits, b_traits, h)
      , EqualHolderT(e)
   {}

   inline BucketHashEqualT(BOOST_RV_REF(BucketHashEqualT) other)
      : BucketHashType(BOOST_MOVE_BASE(BucketHashType, other))
      , EqualHolderT(BOOST_MOVE_BASE(EqualHolderT, other))
   {}

   typedef typename UnorderedBucketPtrImpl
      <typename BucketHashType::value_traits>::type BucketPtr;

   inline BucketPtr priv_get_cache() const
   {  return m_cached_begin;   }

   inline void priv_set_cache(BucketPtr p)
   {  m_cached_begin = p;   }

   inline void priv_set_cache_bucket_num(std::size_t insertion_bucket)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(insertion_bucket <= this->priv_usable_bucket_count());
      this->m_cached_begin = this->priv_bucket_pointer() + std::ptrdiff_t(insertion_bucket);
   }

   inline std::size_t priv_get_cache_bucket_num()
   {  return std::size_t(this->m_cached_begin - this->priv_bucket_pointer());  }

   inline void priv_init_cache()
   {  this->m_cached_begin = this->priv_past_usable_bucket_ptr();  }

   inline void priv_swap_cache(BucketHashEqualT &other)
   {  ::boost::adl_move_swap(this->m_cached_begin, other.m_cached_begin);  }

   Siterator priv_begin(BucketPtr& pbucketptr) const
   {
      pbucketptr = this->m_cached_begin;
      if(this->m_cached_begin == this->priv_past_usable_bucket_ptr()){
         return this->priv_end_sit();
      }
      else{
         return Siterator(m_cached_begin->begin_ptr());
      }
   }

   void priv_insertion_update_cache(std::size_t insertion_bucket)
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(insertion_bucket < this->priv_usable_bucket_count());
      BucketPtr p = this->priv_bucket_pointer() + std::ptrdiff_t(insertion_bucket);
      if(p < this->m_cached_begin){
         this->m_cached_begin = p;
      }
   }

   inline const KeyEqual &priv_equal() const
   {  return this->EqualHolderT::get();  }

   inline KeyEqual &priv_equal()
   {  return this->EqualHolderT::get();  }

   void priv_erasure_update_cache_range(std::size_t first_bucket_num, std::size_t last_bucket_num)
   {
      //If the last bucket is the end, the cache must be updated
      //to the last position if all
      if(this->priv_get_cache_bucket_num() == first_bucket_num   &&
         this->priv_bucket_empty(first_bucket_num) ){
         this->priv_set_cache(this->priv_bucket_pointer() + std::ptrdiff_t(last_bucket_num));
         this->priv_erasure_update_cache();
      }
   }

   void priv_erasure_update_cache(BucketPtr first_bucket)
   {
      //If the last bucket is the end, the cache must be updated
      //to the last position if all
      if (this->priv_get_cache() == first_bucket &&
         this->priv_bucket_empty(first_bucket)) {
         this->priv_erasure_update_cache();
      }
   }

   void priv_erasure_update_cache()
   {
      const BucketPtr cache_end = this->priv_past_usable_bucket_ptr();
      while( m_cached_begin != cache_end) {
         if (!SlistNodeAlgorithms::is_empty(m_cached_begin->get_node_ptr())) {
            return;
         }
         ++m_cached_begin;
      }
   }

   BucketPtr m_cached_begin;
};

//This wrapper around size_traits is used
//to maintain minimal container size with compilers like MSVC
//that have problems with EBO and multiple empty base classes
template<class DeriveFrom, class SizeType, bool>
struct HashtableSizeWrapper
   : public DeriveFrom
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(HashtableSizeWrapper)

   public:
   template<class Base, class Arg0, class Arg1, class Arg2>
   HashtableSizeWrapper( BOOST_FWD_REF(Base) base, BOOST_FWD_REF(Arg0) arg0
                         , BOOST_FWD_REF(Arg1) arg1, BOOST_FWD_REF(Arg2) arg2)
      :  DeriveFrom( ::boost::forward<Base>(base)
                   , ::boost::forward<Arg0>(arg0)
                   , ::boost::forward<Arg1>(arg1)
                   , ::boost::forward<Arg2>(arg2))
   {}
   typedef detail::SizeHolder < true, SizeType> SizeTraits;//size_traits

   inline HashtableSizeWrapper(BOOST_RV_REF(HashtableSizeWrapper) other)
      : DeriveFrom(BOOST_MOVE_BASE(DeriveFrom, other))
   {}

   SizeTraits m_size_traits;

   typedef const SizeTraits & SizeTraitsConstT;
   typedef       SizeTraits & SizeTraitsT;

   inline SizeType get_hashtable_size_wrapper_size() const
   {  return m_size_traits.get_size(); }

   inline void set_hashtable_size_wrapper_size(SizeType s)
   {  m_size_traits.set_size(s); }

   inline void inc_hashtable_size_wrapper_size()
   {  m_size_traits.increment(); }

   inline void dec_hashtable_size_wrapper_size()
   {  m_size_traits.decrement(); }

   inline SizeTraitsT priv_size_traits()
   {  return m_size_traits; }
};

template<class DeriveFrom, class SizeType>
struct HashtableSizeWrapper<DeriveFrom, SizeType, false>
   : public DeriveFrom
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(HashtableSizeWrapper)

   public:
   template<class Base, class Arg0, class Arg1, class Arg2>
   HashtableSizeWrapper( BOOST_FWD_REF(Base) base, BOOST_FWD_REF(Arg0) arg0
                         , BOOST_FWD_REF(Arg1) arg1, BOOST_FWD_REF(Arg2) arg2)
      :  DeriveFrom( ::boost::forward<Base>(base)
                   , ::boost::forward<Arg0>(arg0)
                   , ::boost::forward<Arg1>(arg1)
                   , ::boost::forward<Arg2>(arg2))
   {}

   inline HashtableSizeWrapper(BOOST_RV_REF(HashtableSizeWrapper) other)
      : DeriveFrom(BOOST_MOVE_BASE(DeriveFrom, other))
   {}

   typedef detail::SizeHolder< false, SizeType>   SizeTraits;

   typedef SizeTraits SizeTraitsConstT;
   typedef SizeTraits SizeTraitsT;

   inline SizeType get_hashtable_size_wrapper_size() const
   {  return 0u; }

   inline void set_hashtable_size_wrapper_size(SizeType)
   {}

   inline void inc_hashtable_size_wrapper_size()
   {}

   inline void dec_hashtable_size_wrapper_size()
   {}

   inline SizeTraits priv_size_traits()
   {  return SizeTraits(); }
};

template< class ValueTraits,    class VoidOrKeyOfValue, class VoidOrKeyHash
        , class VoidOrKeyEqual, class BucketTraits,     class SizeType
        , std::size_t BoolFlags>
struct GetHashtableSizeWrapperBucket
{
   typedef HashtableSizeWrapper
      < BucketHashEqualT
         < ValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual
         , BucketTraits
         , 0 != (BoolFlags & HashBoolFlags::LINEAR_BUCKETS_POS)
         , 0 != (BoolFlags & HashBoolFlags::CACHE_BEGIN_POS)
         >   //2
      , SizeType
      , (BoolFlags & HashBoolFlags::INCREMENTAL_POS)     != 0 ||
        (BoolFlags & HashBoolFlags::FASTMOD_BUCKETS_POS) != 0
      > Type;
};

//hashdata_internal
//Stores bucket_hash_equal_t and split_traits
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TBucketTraits, class TSizeType, std::size_t BoolFlags>
struct HashdataInternal
   : public GetHashtableSizeWrapperBucket
               <TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags>::type
{
   private:
   BOOST_MOVABLE_BUT_NOT_COPYABLE(HashdataInternal)

   public:
   static const bool LINEAR_BUCKETS = 0 != (BoolFlags & HashBoolFlags::LINEAR_BUCKETS_POS);
   typedef typename GetHashtableSizeWrapperBucket
      <TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags>::type SplitBucketHashEqualT;
   
   typedef typename SplitBucketHashEqualT::KeyEqual                KeyEqual;
   typedef typename SplitBucketHashEqualT::Hasher                   Hasher;
   typedef BucketPlusVtraits
      <TValueTraits, TBucketTraits, LINEAR_BUCKETS>           BucketPlusVtraitsT;
   typedef TSizeType                                         SizeType;
   typedef typename SplitBucketHashEqualT::size_traits              SplitTraits;
   typedef typename BucketPlusVtraitsT::BucketPtr       BucketPtr;
   typedef typename BucketPlusVtraitsT::ConstValueTraitsPtr   ConstValueTraitsPtr;
   typedef typename BucketPlusVtraitsT::Siterator        Siterator;
   typedef typename BucketPlusVtraitsT::BucketTraits    BucketTraits;
   typedef typename BucketPlusVtraitsT::ValueTraits     ValueTraits;
   typedef typename BucketPlusVtraitsT::BucketType      BucketType;
   typedef typename ValueTraits::ValueType                ValueType;
   typedef typename ValueTraits::Pointer                   Pointer;
   typedef typename ValueTraits::ConstPointer             ConstPointer;
   typedef typename PointerTraits<Pointer>::Reference      Reference;
   typedef typename PointerTraits
      <ConstPointer>::reference                            ConstReference;
   typedef typename ValueTraits::NodeTraits               NodeTraits;
   typedef typename NodeTraits::Node                       Node;
   typedef typename NodeTraits::NodePtr                   NodePtr;
   typedef typename NodeTraits::ConstNodePtr             ConstNodePtr;
   typedef typename BucketPlusVtraitsT::SlistNodeAlgorithms  SlistNodeAlgorithms;
   typedef typename BucketPlusVtraitsT::SlistNodePtr   SlistNodePtr;

   typedef HashKeyTypesBase
      < typename TValueTraits::value_type
      , VoidOrKeyOfValue
      >                                                              HashTypesBase;
   typedef typename HashTypesBase::KeyOfValue                    KeyOfValue;

   static const bool STORE_HASH = StoreHashIsTrue<NodeTraits>::value;
   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;
   static const bool STATEFUL_VALUE_TRAITS = detail::IsStatefulValueTraits<ValueTraits>::value;

   typedef detail::bool_<STORE_HASH>                                 StoreHashT;

   typedef detail::TransformIterator
      < Siterator
      , DowncastNodeToValueT<ValueTraits, false> >              LocalIterator;

   typedef detail::TransformIterator
      < Siterator
      , DowncastNodeToValueT<ValueTraits, true> >               ConstLocalIterator;

   typedef detail::bool_<LINEAR_BUCKETS>                             LinearBucketsT;

   HashdataInternal( const TValueTraits &val_traits, const BucketTraits &b_traits
                    , const Hasher & h, const KeyEqual &e)
      : SplitBucketHashEqualT(val_traits, b_traits, h, e)
   {}

   inline HashdataInternal(BOOST_RV_REF(HashdataInternal) other)
      : SplitBucketHashEqualT(BOOST_MOVE_BASE(SplitBucketHashEqualT, other))
   {}

   inline typename SplitBucketHashEqualT::size_traits_t priv_split_traits()
   {  return this->priv_size_traits();  }

   ~HashdataInternal()
   #if defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
      requires (TValueTraits::link_mode != NormalLink)
   #endif
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
         this->priv_clear_buckets();
      }
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) && defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
   //Default destructor for normal links (allows conditional triviality)
   ~HashdataInternal() requires (TValueTraits::link_mode == NormalLink) = default;
   #endif

   using SplitBucketHashEqualT::priv_clear_buckets;

   void priv_clear_buckets()
   {
      const std::size_t cache_num = this->priv_get_cache_bucket_num();
      this->priv_clear_buckets(this->priv_get_cache(), this->priv_usable_bucket_count() - cache_num);
   }

   void priv_clear_buckets_and_cache()
   {
      this->priv_clear_buckets();
      this->priv_init_cache();
   }

   void priv_init_buckets_and_cache()
   {
      this->priv_init_buckets(this->priv_bucket_pointer(), this->priv_usable_bucket_count());
      this->priv_init_cache();
   }
   
   typedef typename BucketPlusVtraitsT::Iterator         Iterator;
   typedef typename BucketPlusVtraitsT::ConstIterator   ConstIterator;

   //public functions
   inline TSizeType split_count() const BOOST_NOEXCEPT
   {  return this->SplitBucketHashEqualT::get_hashtable_size_wrapper_size();  }

   inline void split_count(TSizeType s) BOOST_NOEXCEPT
   {  this->SplitBucketHashEqualT::set_hashtable_size_wrapper_size(s);  }

   //public functions
   inline void inc_split_count() BOOST_NOEXCEPT
   {  this->SplitBucketHashEqualT::inc_hashtable_size_wrapper_size();  }

   inline void dec_split_count() BOOST_NOEXCEPT
   {  this->SplitBucketHashEqualT::dec_hashtable_size_wrapper_size();  }

   inline static TSizeType initial_split_from_bucket_count(TSizeType bc) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(FASTMOD_BUCKETS) {
         SizeType split;
         split = static_cast<TSizeType>(PrimeFmodSize::lower_size_index(bc));
         //The passed bucket size must be exactly the supported one
         BOOST_ASSERT(PrimeFmodSize::size(split) == bc);
         return split;
      }
      else {
         BOOST_IF_CONSTEXPR(INCREMENTAL) {
            BOOST_ASSERT(0 == (std::size_t(bc) & (std::size_t(bc) - 1u)));
            return SizeType(bc >> 1u);
         }
         else{
            return bc;
         }
      }
   }

   inline static TSizeType rehash_split_from_bucket_count(TSizeType bc) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(FASTMOD_BUCKETS) {
         return (initial_split_from_bucket_count)(bc);
      }
      else {
         BOOST_IF_CONSTEXPR(INCREMENTAL) {
            BOOST_ASSERT(0 == (std::size_t(bc) & (std::size_t(bc) - 1u)));
         }
         return bc;
      }
   }

   inline Iterator iterator_to(Reference value) BOOST_NOEXCEPT_IF(!LINEAR_BUCKETS)
   {  return iterator_to(value, LinearBucketsT());  }

   ConstIterator iterator_to(ConstReference value) const BOOST_NOEXCEPT_IF(!LINEAR_BUCKETS)
   {  return iterator_to(value, LinearBucketsT());  }

   Iterator iterator_to(Reference value, detail::true_)  //linear_buckets
   {
      const std::size_t h = this->priv_stored_or_compute_hash(value, StoreHashT());
      Siterator sit(this->priv_value_to_node_ptr(value));
      return this->build_iterator(sit, this->priv_hash_to_bucket_ptr(h));
   }

   ConstIterator iterator_to(ConstReference value, detail::true_) const //linear_buckets
   {
      const std::size_t h = this->priv_stored_or_compute_hash(value, StoreHashT());
      Siterator const sit = Siterator
         ( PointerTraits<NodePtr>::const_cast_from(this->priv_value_to_node_ptr(value))
         );
      return this->build_const_iterator(sit, this->priv_hash_to_bucket_ptr(h));
   }

   static const bool INCREMENTAL = 0 != (BoolFlags & HashBoolFlags::INCREMENTAL_POS);
   static const bool POWER_2_BUCKETS = INCREMENTAL || (0 != (BoolFlags & HashBoolFlags::POWER_2_BUCKETS_POS));
   static const bool FASTMOD_BUCKETS = 0 != (BoolFlags & HashBoolFlags::FASTMOD_BUCKETS_POS);

   typedef detail::bool_<FASTMOD_BUCKETS> FastmodBucketsT;

   inline BucketType &priv_hash_to_bucket(std::size_t hash_value) const
   {  return this->priv_bucket(this->priv_hash_to_nbucket(hash_value));   }

   inline BucketPtr priv_hash_to_bucket_ptr(std::size_t hash_value) const
   {  return this->priv_bucket_ptr(this->priv_hash_to_nbucket(hash_value));   }

   inline SizeType priv_hash_to_nbucket(std::size_t hash_value) const
   {  return (priv_hash_to_nbucket)(hash_value, FastmodBucketsT());  }

   inline SizeType priv_hash_to_nbucket(std::size_t hash_value, detail::true_) const  //fastmod_buckets_t
   {  return static_cast<SizeType>(PrimeFmodSize::position(hash_value, this->split_count()));  }

   inline SizeType priv_hash_to_nbucket(std::size_t hash_value, detail::false_) const //!fastmod_buckets_t
   {
      return static_cast<SizeType>(hash_to_bucket_split<POWER_2_BUCKETS, INCREMENTAL>
         (hash_value, this->priv_usable_bucket_count(), this->split_count(), detail::false_()));
   }

   inline Iterator iterator_to(Reference value, detail::false_) BOOST_NOEXCEPT
   {
      return Iterator( Siterator(this->priv_value_to_node_ptr(value))
                     , &this->get_bucket_value_traits());
   }

   ConstIterator iterator_to(ConstReference value, detail::false_) const BOOST_NOEXCEPT
   {
      Siterator const sit = Siterator
         ( PointerTraits<NodePtr>::const_cast_from(this->priv_value_to_node_ptr(value)) );
      return ConstIterator(sit, &this->get_bucket_value_traits());
   }


   static LocalIterator s_local_iterator_to(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      Siterator sit(ValueTraits::to_node_ptr(value));
      return LocalIterator(sit, ConstValueTraitsPtr());
   }

   static ConstLocalIterator s_local_iterator_to(ConstReference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      Siterator const sit = Siterator
         ( PointerTraits<NodePtr>::const_cast_from
            (ValueTraits::to_node_ptr(value))
         );
      return ConstLocalIterator(sit, ConstValueTraitsPtr());
   }

   LocalIterator local_iterator_to(Reference value) BOOST_NOEXCEPT
   {
      Siterator sit(this->priv_value_to_node_ptr(value));
      return LocalIterator(sit, this->priv_value_traits_ptr());
   }

   ConstLocalIterator local_iterator_to(ConstReference value) const BOOST_NOEXCEPT
   {
       Siterator sit
         ( PointerTraits<NodePtr>::const_cast_from(this->priv_value_to_node_ptr(value)) );
      return ConstLocalIterator(sit, this->priv_value_traits_ptr());
   }

   inline SizeType bucket_count() const BOOST_NOEXCEPT
   {  return SizeType(this->priv_usable_bucket_count());   }

   inline SizeType bucket_size(SizeType n) const BOOST_NOEXCEPT
   {  return (SizeType)this->priv_bucket_size(n);   }

   inline BucketPtr bucket_pointer() const BOOST_NOEXCEPT
   {  return this->priv_bucket_pointer();   }

   inline LocalIterator begin(SizeType n) BOOST_NOEXCEPT
   {  return LocalIterator(this->priv_bucket_lbegin(n), this->priv_value_traits_ptr());  }

   inline ConstLocalIterator begin(SizeType n) const BOOST_NOEXCEPT
   {  return this->cbegin(n);  }

   static inline SizeType suggested_upper_bucket_count(SizeType n) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(FASTMOD_BUCKETS){
         std::size_t s = PrimeFmodSize::upper_size_index(n);
         return static_cast<TSizeType>(PrimeFmodSize::size(s));
      }
      else{
         return PrimeListHolder<0>::suggested_upper_bucket_count(n);
      }
   }

   static inline SizeType suggested_lower_bucket_count(SizeType n) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(FASTMOD_BUCKETS){
         std::size_t s = PrimeFmodSize::lower_size_index(n);
         return static_cast<TSizeType>(PrimeFmodSize::size(s));
      }
      else{
         return PrimeListHolder<0>::suggested_lower_bucket_count(n);
      }
   }

   ConstLocalIterator cbegin(SizeType n) const BOOST_NOEXCEPT
   {
      return ConstLocalIterator
         (this->priv_bucket_lbegin(n)
         , this->priv_value_traits_ptr());
   }

   using SplitBucketHashEqualT::end;
   using SplitBucketHashEqualT::cend;

   LocalIterator end(SizeType n) BOOST_NOEXCEPT
   {  return LocalIterator(this->priv_bucket_lend(n), this->priv_value_traits_ptr());  }

   inline ConstLocalIterator end(SizeType n) const BOOST_NOEXCEPT
   {  return this->cend(n);  }

   ConstLocalIterator cend(SizeType n) const BOOST_NOEXCEPT
   {
      return ConstLocalIterator
         ( this->priv_bucket_lend(n)
         , this->priv_value_traits_ptr());
   }

   //Public functions for hashtable_impl

   inline Iterator begin() BOOST_NOEXCEPT
   {
      BucketPtr p;
      Siterator s = this->priv_begin(p);
      return this->build_iterator(s, p);
   }

   inline ConstIterator begin() const BOOST_NOEXCEPT
   {  return this->cbegin();  }

   inline ConstIterator cbegin() const BOOST_NOEXCEPT
   {
      BucketPtr p;
      Siterator s = this->priv_begin(p);
      return this->build_const_iterator(s, p);
   }

   inline Hasher hash_function() const
   {  return this->priv_hasher();  }

   inline KeyEqual key_eq() const
   {  return this->priv_equal();   }
};

template< class ValueTraits,    class VoidOrKeyOfValue, class VoidOrKeyHash
        , class VoidOrKeyEqual, class BucketTraits,     class SizeType
        , std::size_t BoolFlags>
struct GetHashtableSizeWrapperInternal
{
   typedef HashtableSizeWrapper
      < HashdataInternal
         < ValueTraits
         , VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual
         , BucketTraits, SizeType
         , BoolFlags & ~(HashBoolFlags::CONSTANT_TIME_SIZE_POS) //1
         >
      , SizeType
      , (BoolFlags& HashBoolFlags::CONSTANT_TIME_SIZE_POS) != 0
      > Type;
};

/// @endcond

//! The class template hashtable is an intrusive hash table container, that
//! is used to construct intrusive unordered_set and unordered_multiset containers. The
//! no-throw guarantee holds only, if the VoidOrKeyEqual object and Hasher don't throw.
//!
//! hashtable is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: hashtable needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! hashtable more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, power_2_buckets<>, cache_begin<> and incremental<>.
//!
//! hashtable only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size hashtables because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant_time size hashtables are mainly provided to support auto-unlink hooks.
//!
//! hashtables, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehashing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TBucketTraits, class TSizeType, std::size_t BoolFlags>
#endif
class HashtableImpl
   : private GetHashtableSizeWrapperInternal
         <TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags>::type
{
   static const bool LINEAR_BUCKETS_FLAG = (BoolFlags & HashBoolFlags::LINEAR_BUCKETS_POS) != 0;
   typedef typename GetHashtableSizeWrapperInternal
      <TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags>::type
      InternalType;
   typedef typename InternalType::SizeTraits                       SizeTraits;
   typedef HashKeyTypesBase
      < typename TValueTraits::value_type
      , VoidOrKeyOfValue
      >                                                              HashTypesBase;
   public:
   typedef TValueTraits  ValueTraits;

   /// @cond
   typedef TBucketTraits                                              BucketTraits;

   typedef BucketPlusVtraits
         <TValueTraits, TBucketTraits, LINEAR_BUCKETS_FLAG>            BucketPlusVtraitsT;
   typedef typename BucketPlusVtraitsT::ConstValueTraitsPtr    ConstValueTraitsPtr;

   typedef detail::bool_<LINEAR_BUCKETS_FLAG>                        LinearBucketsT;

   typedef typename InternalType::Siterator                         Siterator;
   typedef typename InternalType::ConstSiterator                   ConstSiterator;
   using InternalType::begin;
   using InternalType::cbegin;
   using InternalType::end;
   using InternalType::cend;
   using InternalType::hash_function;
   using InternalType::key_eq;
   using InternalType::bucket_size;
   using InternalType::bucket_count;
   using InternalType::local_iterator_to;
   using InternalType::s_local_iterator_to;
   using InternalType::iterator_to;
   using InternalType::bucket_pointer;
   using InternalType::suggested_upper_bucket_count;
   using InternalType::suggested_lower_bucket_count;
   using InternalType::split_count;

   /// @endcond

   typedef typename ValueTraits::Pointer                            Pointer;
   typedef typename ValueTraits::ConstPointer                      ConstPointer;
   typedef typename ValueTraits::ValueType                         ValueType;
   typedef typename HashTypesBase::KeyType                        KeyType;
   typedef typename HashTypesBase::KeyOfValue                    KeyOfValue;
   typedef typename PointerTraits<Pointer>::Reference               Reference;
   typedef typename PointerTraits<ConstPointer>::reference         ConstReference;
   typedef typename PointerTraits<Pointer>::DifferenceType         DifferenceType;
   typedef TSizeType                                                  SizeType;
   typedef typename InternalType::KeyEqual                         KeyEqual;
   typedef typename InternalType::Hasher                            Hasher;
   typedef typename InternalType::BucketType                       BucketType;
   typedef typename InternalType::BucketPtr                        BucketPtr;
   typedef typename InternalType::Iterator                          Iterator;
   typedef typename InternalType::ConstIterator                    ConstIterator;
   typedef typename InternalType::LocalIterator                    LocalIterator;
   typedef typename InternalType::ConstLocalIterator              ConstLocalIterator;
   typedef typename ValueTraits::NodeTraits                        NodeTraits;
   typedef typename NodeTraits::Node                                Node;
   typedef typename PointerTraits
      <Pointer>::template rebind_pointer
         < Node >::type                                              NodePtr;
   typedef typename PointerTraits
      <Pointer>::template rebind_pointer
         < const Node >::type                                        ConstNodePtr;
   typedef typename PointerTraits
      <NodePtr>::reference                                          NodeReference;
   typedef typename PointerTraits
      <ConstNodePtr>::reference                                    ConstNodeReference;
   typedef typename InternalType::SlistNodeAlgorithms             SlistNodeAlgorithms;

   static const bool STATEFUL_VALUE_TRAITS = InternalType::STATEFUL_VALUE_TRAITS;
   static const bool STORE_HASH = InternalType::STORE_HASH;

   static const bool UNIQUE_KEYS          = 0 != (BoolFlags & HashBoolFlags::UNIQUE_KEYS_POS);
   static const bool CONSTANT_TIME_SIZE   = 0 != (BoolFlags & HashBoolFlags::CONSTANT_TIME_SIZE_POS);
   static const bool CACHE_BEGIN          = 0 != (BoolFlags & HashBoolFlags::CACHE_BEGIN_POS);
   static const bool COMPARE_HASH         = 0 != (BoolFlags & HashBoolFlags::COMPARE_HASH_POS);
   static const bool INCREMENTAL          = 0 != (BoolFlags & HashBoolFlags::INCREMENTAL_POS);
   static const bool POWER_2_BUCKETS      = INCREMENTAL || (0 != (BoolFlags & HashBoolFlags::POWER_2_BUCKETS_POS));
   static const bool OPTIMIZE_MULTIKEY    = OptimizeMultikeyIsTrue<NodeTraits>::value && !UNIQUE_KEYS;
   static const bool LINEAR_BUCKETS       = LINEAR_BUCKETS_FLAG;
   static const bool FASTMOD_BUCKETS      = 0 != (BoolFlags & HashBoolFlags::FASTMOD_BUCKETS_POS);
   static const std::size_t BUCKET_OVERHEAD = InternalType::BUCKET_OVERHEAD;

   /// @cond
   static const bool IS_MULTIKEY = !UNIQUE_KEYS;
   private:

   //Configuration error: compare_hash<> can't be specified without store_hash<>
   //See documentation for more explanations
   BOOST_INTRUSIVE_STATIC_ASSERT((!COMPARE_HASH || STORE_HASH));

   //Configuration error: fasmod_buckets<> can't be specified with incremental<> or power_2_buckets<>
   //See documentation for more explanations
   BOOST_INTRUSIVE_STATIC_ASSERT(!(FASTMOD_BUCKETS && POWER_2_BUCKETS));

   typedef typename InternalType::SlistNodePtr                    SlistNodePtr;
   typedef typename PointerTraits
      <SlistNodePtr>::template rebind_pointer
         < void >::type                                              VoidPointer;
   //We'll define group traits, but these won't be instantiated if
   //optimize_multikey is not true
   typedef UnorderedGroupAdapter<NodeTraits>                      GroupTraits;
   typedef CircularSlistAlgorithms<GroupTraits>                   GroupAlgorithms;
   typedef typename InternalType::StoreHashT                      StoreHashT;
   typedef detail::bool_<OPTIMIZE_MULTIKEY>                          OptimizeMultikeyT;
   typedef detail::bool_<CACHE_BEGIN>                                CacheBeginT;
   typedef detail::bool_<POWER_2_BUCKETS>                            Power2BucketsT;
   typedef detail::bool_<FASTMOD_BUCKETS>                            FastmodBucketsT;
   typedef detail::bool_<COMPARE_HASH>                               CompareHashT;
   typedef typename InternalType::SplitTraits                      SplitTraits;
   typedef GroupFunctions<NodeTraits>                              GroupFunctionsT;
   typedef NodeFunctions<NodeTraits>                               NodeFunctionsT;

   private:
   //noncopyable, movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(HashtableImpl)

   static const bool SAFEMODE_OR_AUTOUNLINK = InternalType::SAFEMODE_OR_AUTOUNLINK;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CONSTANT_TIME_SIZE && ((int)ValueTraits::link_mode == (int)AutoUnlink)));
   //Cache begin is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CACHE_BEGIN && ((int)ValueTraits::link_mode == (int)AutoUnlink)));


   /// @endcond
   
   public:
   typedef InsertCommitDataImpl InsertCommitData;

   private:
   void default_init_actions()
   {
      this->priv_set_sentinel_bucket();
      this->priv_init_buckets_and_cache();
      this->priv_size_count(SizeType(0));
      SizeType bucket_sz = this->bucket_count();
      BOOST_INTRUSIVE_INVARIANT_ASSERT(bucket_sz != 0);
      //Check power of two bucket array if the option is activated
      BOOST_INTRUSIVE_INVARIANT_ASSERT
         (!POWER_2_BUCKETS || (0 == (bucket_sz & (bucket_sz - 1))));
      this->split_count(this->initial_split_from_bucket_count(bucket_sz));
   }

   inline TSizeType priv_size_count() const BOOST_NOEXCEPT
   {  return this->InternalType::get_hashtable_size_wrapper_size(); }

   inline void priv_size_count(TSizeType s) BOOST_NOEXCEPT
   {  this->InternalType::set_hashtable_size_wrapper_size(s); }

   inline void priv_size_inc() BOOST_NOEXCEPT
   {  this->InternalType::inc_hashtable_size_wrapper_size(); }

   inline void priv_size_dec() BOOST_NOEXCEPT
   {  this->InternalType::dec_hashtable_size_wrapper_size(); }

   public:

   //! <b>Requires</b>: buckets must not be being used by any other resource.
   //!
   //! <b>Effects</b>: Constructs an empty unordered_set, storing a reference
   //!   to the bucket array and copies of the key_hasher and equal_func functors.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of hash_func or equal_func throws.
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed.
   explicit HashtableImpl ( const BucketTraits &b_traits
                           , const Hasher & hash_func = Hasher()
                           , const KeyEqual &equal_func = KeyEqual()
                           , const ValueTraits &v_traits = ValueTraits())
      :  InternalType(v_traits, b_traits, hash_func, equal_func)
   {  this->default_init_actions(); }

   //! <b>Requires</b>: buckets must not be being used by any other resource
   //!   and dereferencing iterator must yield an lvalue of type value_type.
   //!
   //! <b>Effects</b>: Constructs an empty container and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: If N is distance(b, e): Average case is O(N)
   //!   (with a good hash function and with buckets_len >= N),worst case O(N^2).
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor or invocation of hasher or key_equal throws.
   //!
   //! <b>Notes</b>: buckets array must be disposed only after
   //!   *this is disposed.
   template<class Iterator>
   HashtableImpl ( bool unique, Iterator b, Iterator e
                  , const BucketTraits &b_traits
                  , const Hasher & hash_func = Hasher()
                  , const KeyEqual &equal_func = KeyEqual()
                  , const ValueTraits &v_traits = ValueTraits())
      :  InternalType(v_traits, b_traits, hash_func, equal_func)
   {
      this->default_init_actions();

      //Now insert
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! <b>Effects</b>: Constructs a container moving resources from another container.
   //!   Internal value traits, bucket traits, hasher and comparison are move constructed and
   //!   nodes belonging to x are linked to *this.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node's
   //!   move constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the move constructor of value traits, bucket traits, hasher or comparison throws.
   HashtableImpl(BOOST_RV_REF(HashtableImpl) x)
      : InternalType(BOOST_MOVE_BASE(InternalType, x))
   {
      this->priv_swap_cache(x);
      x.priv_init_cache();
      this->priv_size_count(x.priv_size_count());
      x.priv_size_count(SizeType(0));
      this->split_count(x.split_count());
      x.split_count(SizeType(0));
   }

   //! <b>Effects</b>: Equivalent to swap.
   //!
   HashtableImpl& operator=(BOOST_RV_REF(HashtableImpl) x)
   {  this->swap(x); return *this;  }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Detaches all elements from this. The objects in the unordered_set
   //!   are not deleted (i.e. no destructors are called).
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the unordered_set, if
   //!   it's a safe-mode or auto-unlink value. Otherwise constant.
   //!
   //! <b>Throws</b>: Nothing.
   ~HashtableImpl();

   //! <b>Effects</b>: Returns an iterator pointing to the beginning of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   iterator begin() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator begin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the beginning
   //!   of the unordered_set.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!   Worst case (empty unordered_set): O(this->bucket_count())
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns an iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   iterator end() BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator end() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns a const_iterator pointing to the end of the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_iterator cend() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns the hasher object used by the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If hasher copy-constructor throws.
   hasher hash_function() const;

   //! <b>Effects</b>: Returns the key_equal object used by the unordered_set.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If key_equal copy-constructor throws.
   key_equal key_eq() const;

   #endif

   //! <b>Effects</b>: Returns true if the container is empty.
   //!
   //! <b>Complexity</b>: if constant-time size and cache_begin options are disabled,
   //!   average constant time (worst case, with empty() == true: O(this->bucket_count()).
   //!   Otherwise constant.
   //!
   //! <b>Throws</b>: Nothing.
   [[nodiscard]] bool empty() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
         return !this->size();
      }
      else BOOST_IF_CONSTEXPR(CACHE_BEGIN){
         return this->begin() == this->end();
      }
      else{
         SizeType bucket_cnt = this->bucket_count();
         const BucketType *b = boost::movelib::to_raw_pointer(this->priv_bucket_pointer());
         for (SizeType n = 0; n < bucket_cnt; ++n, ++b){
            if(!SlistNodeAlgorithms::is_empty(b->get_node_ptr())){
               return false;
            }
         }
         return true;
      }
   }

   //! <b>Effects</b>: Returns the number of elements stored in the unordered_set.
   //!
   //! <b>Complexity</b>: Linear to elements contained in *this if
   //!   constant_time_size is false. Constant-time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   SizeType size() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         return this->priv_size_count();
      else{
         std::size_t len = 0;
         std::size_t bucket_cnt = this->bucket_count();
         const BucketType *b = boost::movelib::to_raw_pointer(this->priv_bucket_pointer());
         for (std::size_t n = 0; n < bucket_cnt; ++n, ++b){
            len += SlistNodeAlgorithms::count(b->get_node_ptr()) - 1u;
         }
         BOOST_INTRUSIVE_INVARIANT_ASSERT((len <= TSizeType(-1)));
         return SizeType(len);
      }
   }

   //! <b>Requires</b>: the hasher and the equality function unqualified swap
   //!   call should not throw.
   //!
   //! <b>Effects</b>: Swaps the contents of two unordered_sets.
   //!   Swaps also the contained bucket array and equality and hasher functors.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the swap() call for the comparison or hash functors
   //!   found using ADL throw. Basic guarantee.
   void swap(HashtableImpl& other)
   {
      //These can throw
      ::boost::adl_move_swap(this->priv_equal(),  other.priv_equal());
      ::boost::adl_move_swap(this->priv_hasher(), other.priv_hasher());
      //These can't throw
      ::boost::adl_move_swap(this->priv_bucket_traits(), other.priv_bucket_traits());
      ::boost::adl_move_swap(this->priv_value_traits(), other.priv_value_traits());
      this->priv_swap_cache(other);
      this->priv_size_traits().swap(other.priv_size_traits());
      this->priv_split_traits().swap(other.priv_split_traits());
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw
   //!   Cloner should yield to nodes that compare equal and produce the same
   //!   hash than the original node.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. The hash function and the equality
   //!   predicate are copied from the source.
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!
   //!   If any operation throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner or hasher throw or hash or equality predicate copying
   //!   throws. Basic guarantee.
   template <class Cloner, class Disposer>
   inline void clone_from(const HashtableImpl &src, Cloner cloner, Disposer disposer)
   {  this->priv_clone_from(src, cloner, disposer);   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw
   //!   Cloner should yield to nodes that compare equal and produce the same
   //!   hash than the original node.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(reference)
   //!   and inserts them on *this. The hash function and the equality
   //!   predicate are copied from the source.
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!
   //!   If any operation throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner or hasher throw or hash or equality predicate copying
   //!   throws. Basic guarantee.
   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(HashtableImpl) src, Cloner cloner, Disposer disposer)
   {  this->priv_clone_from(static_cast<HashtableImpl&>(src), cloner, disposer);   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts the value into the unordered_set.
   //!
   //! <b>Returns</b>: An iterator to the inserted value.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_equal(Reference value)
   {
      SizeType bucket_num;
      std::size_t hash_value;
      Siterator prev;
      Siterator const it = this->priv_find
         (KeyOfValue()(value), this->priv_hasher(), this->priv_equal(), bucket_num, hash_value, prev);
      bool const next_is_in_group = OPTIMIZE_MULTIKEY && it != this->priv_end_sit();
      return this->priv_insert_equal_after_find(value, bucket_num, hash_value, prev, next_is_in_group);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Equivalent to this->insert_equal(t) for each element in [b, e).
   //!
   //! <b>Complexity</b>: Average case O(N), where N is distance(b, e).
   //!   Worst case O(N*this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      for (; b != e; ++b)
         this->insert_equal(*b);
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Tries to inserts value into the unordered_set.
   //!
   //! <b>Returns</b>: If the value
   //!   is not already present inserts it and returns a pair containing the
   //!   iterator to the new value and true. If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<Iterator, bool> insert_unique(Reference value)
   {
      InsertCommitData commit_data;
      std::pair<Iterator, bool> ret = this->insert_unique_check(KeyOfValue()(value), commit_data);
      if(ret.second){
         ret.first = this->insert_unique_fast_commit(value, commit_data);
      }
      return ret;
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Equivalent to this->insert_unique(t) for each element in [b, e).
   //!
   //! <b>Complexity</b>: Average case O(N), where N is distance(b, e).
   //!   Worst case O(N*this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws. Basic guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      for (; b != e; ++b)
         this->insert_unique(*b);
   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the unordered_set, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal_func throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the hash or the equality is much cheaper to
   //!   construct than the value_type and this function offers the possibility to
   //!   use that the part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time.
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the unordered_set.
   //!
   //!   After a successful rehashing insert_commit_data remains valid.
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<Iterator, bool> insert_unique_check
      ( const KeyType &key
      , KeyHasher hash_func
      , KeyEqual equal_func
      , InsertCommitData &commit_data)
   {
      const std::size_t h = hash_func(key);
      const std::size_t bn = this->priv_hash_to_nbucket(h);

      commit_data.m_bucket_idx = bn;
      commit_data.set_hash(h);

      BucketPtr bp = this->priv_bucket_ptr(bn);
      Siterator const s = this->priv_find_in_bucket(*bp, key, equal_func, h);
      return std::pair<Iterator, bool>(this->build_iterator(s, bp), s == this->priv_end_sit());
   }

   //! <b>Effects</b>: Checks if a value can be inserted in the unordered_set, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hasher or key_compare throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the hash or the equality is much cheaper to
   //!   construct than the value_type and this function offers the possibility to
   //!   use that the part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time.
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the unordered_set.
   //!
   //!   After a successful rehashing insert_commit_data remains valid.
   inline std::pair<Iterator, bool> insert_unique_check
      ( const KeyType &key, InsertCommitData &commit_data)
   {  return this->insert_unique_check(key, this->priv_hasher(), this->priv_equal(), commit_data);  }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the unordered_set between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //! <b>Effects</b>: Inserts the value in the unordered_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   //!
   //!   After a successful rehashing insert_commit_data remains valid.
   Iterator insert_unique_commit(Reference value, const InsertCommitData& commit_data) BOOST_NOEXCEPT
   {
      SizeType bucket_num = this->priv_hash_to_nbucket(commit_data.get_hash());
      BucketType& b = this->priv_bucket(bucket_num);
      this->priv_size_traits().increment();
      NodePtr const n = PointerTraits<NodePtr>::pointer_to(this->priv_value_to_node(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || SlistNodeAlgorithms::unique(n));
      NodeFunctionsT::store_hash(n, commit_data.get_hash(), StoreHashT());
      this->priv_insertion_update_cache(bucket_num);
      GroupFunctionsT::insert_in_group(n, n, OptimizeMultikeyT());
      SlistNodeAlgorithms::link_after(b.get_node_ptr(), n);
      return this->build_iterator(Siterator(n), this->to_ptr(b));
   }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the unordered_set between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //!   No rehashing shall be performed between `insert_check` and `insert_fast_commit`.
   //! 
   //! <b>Effects</b>: Inserts the value in the unordered_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   //!
   //!   Since this commit operation does not support rehashing between the check
   //!   and the commit, it's faster than `insert_commit`.
   Iterator insert_unique_fast_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      this->priv_size_inc();
      NodePtr const n = this->priv_value_to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || SlistNodeAlgorithms::unique(n));
      NodeFunctionsT::store_hash(n, commit_data.get_hash(), StoreHashT());
      this->priv_insertion_update_cache(static_cast<SizeType>(commit_data.m_bucket_idx));
      GroupFunctionsT::insert_in_group(n, n, OptimizeMultikeyT());
      BucketType& b = this->priv_bucket(commit_data.m_bucket_idx);
      SlistNodeAlgorithms::link_after(b.get_node_ptr(), n);
      return this->build_iterator(Siterator(n), this->to_ptr(b));
   }

   //! <b>Effects</b>: Erases the element pointed to by i.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased element. No destructors are called.
   inline void erase(ConstIterator i) BOOST_NOEXCEPT
   {  this->erase_and_dispose(i, detail::NullDisposer());  }

   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!
   //! <b>Complexity</b>: Average case O(distance(b, e)),
   //!   worst case O(this->size()).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   inline void erase(ConstIterator b, ConstIterator e) BOOST_NOEXCEPT
   {  this->erase_and_dispose(b, e, detail::NullDisposer());  }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!   Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   inline SizeType erase(const KeyType &key)
   {  return this->erase(key, this->priv_hasher(), this->priv_equal());  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Erases all the elements that have the same hash and
   //!   compare equal with the given key.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal_func throw. Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyHasher, class KeyEqual>
   inline SizeType erase(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func)
   {  return this->erase_and_dispose(key, hash_func, equal_func, detail::NullDisposer()); }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by i.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   BOOST_INTRUSIVE_DOC1ST(void
      , typename detail::disable_if_convertible<Disposer BOOST_INTRUSIVE_I ConstIterator>::type)
    erase_and_dispose(ConstIterator i, Disposer disposer) BOOST_NOEXCEPT
   {
      //Get the bucket number and local iterator for both iterators
      const BucketPtr bp = this->priv_get_bucket_ptr(i);
      this->priv_erase_node(*bp, i.slist_it(), this->make_node_disposer(disposer), OptimizeMultikeyT());
      this->priv_size_dec();
      this->priv_erasure_update_cache(bp);
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Complexity</b>: Average case O(distance(b, e)),
   //!   worst case O(this->size()).
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   void erase_and_dispose(ConstIterator b, ConstIterator e, Disposer disposer) BOOST_NOEXCEPT
   {
      if(b != e){
         //Get the bucket number and local iterator for both iterators
         SizeType first_bucket_num = this->priv_get_bucket_num(b);

         Siterator before_first_local_it
            = this->priv_get_previous(this->priv_bucket(first_bucket_num), b.slist_it(), OptimizeMultikeyT());
         SizeType last_bucket_num;
         Siterator last_local_it;

         //For the end iterator, we will assign the end iterator
         //of the last bucket
         if(e == this->end()){
            last_bucket_num   = SizeType(this->bucket_count() - 1u);
            last_local_it     = this->sit_end(this->priv_bucket(last_bucket_num));
         }
         else{
            last_local_it     = e.slist_it();
            last_bucket_num   = this->priv_get_bucket_num(e);
         }
         SizeType const num_erased = (SizeType)this->priv_erase_node_range
            ( before_first_local_it, first_bucket_num, last_local_it, last_bucket_num
            , this->make_node_disposer(disposer), OptimizeMultikeyT());
         this->priv_size_count(SizeType(this->priv_size_count()-num_erased));
         this->priv_erasure_update_cache_range(first_bucket_num, last_bucket_num);
      }
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   //!   Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   inline SizeType erase_and_dispose(const KeyType &key, Disposer disposer)
   {  return this->erase_and_dispose(key, this->priv_hasher(), this->priv_equal(), disposer);   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "equal_func".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal_func throw. Basic guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyHasher, class KeyEqual, class Disposer>
   SizeType erase_and_dispose(const KeyType& key, KeyHasher hash_func
                              ,KeyEqual equal_func, Disposer disposer)
   {
      SizeType bucket_num;
      std::size_t h;
      Siterator prev;
      Siterator it = this->priv_find(key, hash_func, equal_func, bucket_num, h, prev);
      bool const success = it != this->priv_end_sit();

      std::size_t cnt(0);
      if(success){
         BOOST_IF_CONSTEXPR(OPTIMIZE_MULTIKEY){
            Siterator past_last_in_group = it;
            (priv_go_to_last_in_group)(past_last_in_group, OptimizeMultikeyT());
            ++past_last_in_group;
            cnt = this->priv_erase_from_single_bucket
               ( this->priv_bucket(bucket_num), prev
               , past_last_in_group
               , this->make_node_disposer(disposer), OptimizeMultikeyT());
         }
         else{
            Siterator const end_sit = this->priv_bucket_lend(bucket_num);
            do{
               ++cnt;
               ++it;
            }while(it != end_sit && 
                  this->priv_is_value_equal_to_key
                  (this->priv_value_from_siterator(it), h, key, equal_func, CompareHashT()));
            SlistNodeAlgorithms::unlink_after_and_dispose(prev.pointed_node(), it.pointed_node(), this->make_node_disposer(disposer));
         }
         this->priv_size_count(SizeType(this->priv_size_count()-cnt));
         this->priv_erasure_update_cache();
      }

      return static_cast<SizeType>(cnt);
   }

   //! <b>Effects</b>: Erases all of the elements.
   //!
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   void clear() BOOST_NOEXCEPT
   {
      this->priv_clear_buckets_and_cache();
      this->priv_size_count(SizeType(0));
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all of the elements.
   //!
   //! <b>Complexity</b>: Linear to the number of elements on the container.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      if(!CONSTANT_TIME_SIZE || !this->empty()){
         SizeType num_buckets = this->bucket_count();
         BucketPtr b = this->priv_bucket_pointer();
         typename InternalType::template typeof_node_disposer<Disposer>::type d(disposer, &this->priv_value_traits());
         for(; num_buckets; ++b){
            --num_buckets;
            SlistNodeAlgorithms::detach_and_dispose(b->get_node_ptr(), d);
         }
         this->priv_size_count(SizeType(0));
      }
      this->priv_init_cache();
   }

   //! <b>Effects</b>: Returns the number of contained elements with the given value
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   inline SizeType count(const KeyType &key) const
   {  return this->count(key, this->priv_hasher(), this->priv_equal());  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns the number of contained elements with the given key
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal throw.
   template<class KeyType, class KeyHasher, class KeyEqual>
   SizeType count(const KeyType &key, KeyHasher hash_func, KeyEqual equal_func) const
   {
      SizeType cnt;
      SizeType n_bucket;
      this->priv_local_equal_range(key, hash_func, equal_func, n_bucket, cnt);
      return cnt;
   }

   //! <b>Effects</b>: Finds an iterator to the first element is equal to
   //!   "value" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   inline Iterator find(const KeyType &key)
   {  return this->find(key, this->priv_hasher(), this->priv_equal());   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   "key" according to the given hash and equality functor or end() if
   //!   that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyEqual>
   Iterator find(const KeyType &key, KeyHasher hash_func, KeyEqual equal_func)
   {
      std::size_t h = hash_func(key);
      BucketPtr bp = this->priv_hash_to_bucket_ptr(h);
      Siterator s = this->priv_find_in_bucket(*bp, key, equal_func, h);
      return this->build_iterator(s, bp);
   }

   //! <b>Effects</b>: Finds a const_iterator to the first element whose key is
   //!   "key" or end() if that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   inline ConstIterator find(const KeyType &key) const
   {  return this->find(key, this->priv_hasher(), this->priv_equal());   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Finds an iterator to the first element whose key is
   //!   "key" according to the given hasher and equality functor or end() if
   //!   that element does not exist.
   //!
   //! <b>Complexity</b>: Average case O(1), worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyEqual>
   ConstIterator find
      (const KeyType &key, KeyHasher hash_func, KeyEqual equal_func) const
   {
      std::size_t h = hash_func(key);
      BucketPtr bp = this->priv_hash_to_bucket_ptr(h);
      Siterator s = this->priv_find_in_bucket(*bp, key, equal_func, h);
      return this->build_const_iterator(s, bp);
   }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   inline std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->equal_range(key, this->priv_hasher(), this->priv_equal());  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, equal_func)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If hash_func or the equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<Iterator,Iterator> equal_range
      (const KeyType &key, KeyHasher hash_func, KeyEqual equal_func)
   {
      PrivEqualRangeResult ret =
         this->priv_equal_range(key, hash_func, equal_func);
      return std::pair<Iterator, Iterator>
         ( this->build_iterator(ret.m_first, ret.m_bucket_first)
         , this->build_iterator(ret.m_second, ret.m_bucket_second));
   }

   //! <b>Effects</b>: Returns a range containing all elements with values equivalent
   //!   to value. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(value)). Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the internal hasher or the equality functor throws.
   inline std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->equal_range(key, this->priv_hasher(), this->priv_equal());  }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //!   "equal_func" must be a equality function that induces
   //!   the same equality as key_equal. The difference is that
   //!   "equal_func" compares an arbitrary key with the contained values.
   //!
   //! <b>Effects</b>: Returns a range containing all elements with equivalent
   //!   keys. Returns std::make_pair(this->end(), this->end()) if no such
   //!   elements exist.
   //!
   //! <b>Complexity</b>: Average case O(this->count(key, hash_func, equal_func)).
   //!   Worst case O(this->size()).
   //!
   //! <b>Throws</b>: If the hasher or equal_func throw.
   //!
   //! <b>Note</b>: This function is used when constructing a value_type
   //!   is expensive and the value_type can be compared with a cheaper
   //!   key type. Usually this key is part of the value_type.
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<ConstIterator,ConstIterator> equal_range
      (const KeyType &key, KeyHasher hash_func, KeyEqual equal_func) const
   {
      PrivEqualRangeResult ret =
         this->priv_equal_range(key, hash_func, equal_func);
      return std::pair<ConstIterator, ConstIterator>
         ( this->build_const_iterator(ret.m_first,  ret.m_bucket_first)
         , this->build_const_iterator(ret.m_second, ret.m_bucket_second));
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid iterator belonging to the unordered_set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the internal hash function throws.
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_iterator belonging to the
   //!   unordered_set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the internal hash function throws.
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static local_iterator s_local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static const_local_iterator s_local_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid local_iterator belonging to the unordered_set
   //!   that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   local_iterator local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! <b>Requires</b>: value must be an lvalue and shall be in a unordered_set of
   //!   appropriate type. Otherwise the behavior is undefined.
   //!
   //! <b>Effects</b>: Returns: a valid const_local_iterator belonging to
   //!   the unordered_set that points to the value
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   const_local_iterator local_iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns the number of buckets passed in the constructor
   //!   or the last rehash function.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   size_type bucket_count() const BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns the number of elements in the nth bucket.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   size_type bucket_size(size_type n) const BOOST_NOEXCEPT;
   #endif  //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the hash functor throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   inline SizeType bucket(const KeyType& k) const
   {  return this->priv_hash_to_nbucket(this->priv_hash(k));   }

   //! <b>Requires</b>: "hash_func" must be a hash function that induces
   //!   the same hash values as the stored hasher. The difference is that
   //!   "hash_func" hashes the given key instead of the value_type.
   //!
   //! <b>Effects</b>: Returns the index of the bucket in which elements
   //!   with keys equivalent to k would be found, if any such element existed.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If hash_func throws.
   //!
   //! <b>Note</b>: the return value is in the range [0, this->bucket_count()).
   template<class KeyType, class KeyHasher>
   inline SizeType bucket(const KeyType& k, KeyHasher hash_func)  const
   {  return this->priv_hash_to_nbucket(hash_func(k));   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! <b>Effects</b>: Returns the bucket array pointer passed in the constructor
   //!   or the last rehash function.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   bucket_ptr bucket_pointer() const BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   local_iterator begin(size_type n) BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   const_local_iterator begin(size_type n) const BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the beginning
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   const_local_iterator cbegin(size_type n) const BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   local_iterator end(size_type n) BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   const_local_iterator end(size_type n) const BOOST_NOEXCEPT;

   //! <b>Requires</b>: n is in the range [0, this->bucket_count()).
   //!
   //! <b>Effects</b>: Returns a const_local_iterator pointing to the end
   //!   of the sequence stored in the bucket n.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>:  [this->begin(n), this->end(n)) is a valid range
   //!   containing all of the elements in the nth bucket.
   const_local_iterator cend(size_type n) const BOOST_NOEXCEPT;
   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Requires</b>: new_bucket_traits can hold a pointer to a new bucket array
   //!   or the same as the old bucket array with a different length. new_size is the length of the
   //!   the array pointed by new_buckets. If new_bucket_traits.bucket_begin() == this->bucket_pointer()
   //!   new_bucket_traits.bucket_count() can be bigger or smaller than this->bucket_count().
   //!   'new_bucket_traits' copy constructor should not throw.
   //!
   //! <b>Effects</b>:
   //!   If `new_bucket_traits.bucket_begin() == this->bucket_pointer()` is false,
   //!   unlinks values from the old bucket and inserts then in the new one according
   //!   to the hash value of values.
   //!
   //!   If `new_bucket_traits.bucket_begin() == this->bucket_pointer()` is true,
   //!   the implementations avoids moving values as much as possible.
   //!
   //!   Bucket traits hold by *this is assigned from new_bucket_traits.
   //!   If the container is configured as incremental<>, the split bucket is set
   //!   to the new bucket_count().
   //!
   //!   If store_hash option is true, this method does not use the hash function.
   //!   If false, the implementation tries to minimize calls to the hash function
   //!	 (e.g. once for equivalent values if optimize_multikey<true> is true).
   //!
   //!   If rehash is successful updates the internal bucket_traits with new_bucket_traits.
   //!
   //! <b>Complexity</b>: Average case linear in this->size(), worst case quadratic.
   //!
   //! <b>Throws</b>: If the hasher functor throws. Basic guarantee.
   inline void rehash(const BucketTraits &new_bucket_traits)
   {  this->priv_rehash_impl(new_bucket_traits, false); }

   //! <b>Note</b>: This function is used when keys from inserted elements are changed 
   //!  (e.g. a language change when key is a string) but uniqueness and hash properties are
   //!  preserved so a fast full rehash recovers invariants for *this without extracting and
   //!  reinserting all elements again.
   //!
   //! <b>Requires</b>: Calls produced to the hash function should not alter the value uniqueness
   //!  properties of already inserted elements. If hasher(key1) == hasher(key2) was true when
   //!  elements were inserted, it shall be true during calls produced in the execution of this function.
   //!
   //!  key_equal is not called inside this function so it is assumed that key_equal(value1, value2)
   //!  should produce the same results as before for inserted elements.
   //!
   //! <b>Effects</b>: Reprocesses all values hold by *this, recalculating their hash values
   //!   and redistributing them though the buckets.
   //!
   //!   If store_hash option is true, this method uses the hash function and updates the stored hash value.
   //!
   //! <b>Complexity</b>: Average case linear in this->size(), worst case quadratic.
   //!
   //! <b>Throws</b>: If the hasher functor throws. Basic guarantee.
   inline void full_rehash()
   {  this->priv_rehash_impl(this->priv_bucket_traits(), true);  }

   //! <b>Requires</b>:
   //!
   //! <b>Effects</b>:
   //!
   //! <b>Complexity</b>:
   //!
   //! <b>Throws</b>:
   //!
   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(bool grow = true)
   {
      //This function is only available for containers with incremental hashing
      BOOST_INTRUSIVE_STATIC_ASSERT(( INCREMENTAL && POWER_2_BUCKETS ));
      const std::size_t split_idx  = this->split_count();
      const std::size_t bucket_cnt = this->bucket_count();
      bool ret = false;

      if(grow){
         //Test if the split variable can be changed
         if((ret = split_idx < bucket_cnt)){
            const std::size_t bucket_to_rehash = split_idx - bucket_cnt/2u;
            BucketType &old_bucket = this->priv_bucket(bucket_to_rehash);
            this->inc_split_count();

            //Anti-exception stuff: if an exception is thrown while
            //moving elements from old_bucket to the target bucket, all moved
            //elements are moved back to the original one.
            IncrementalRehashRollback<BucketType, SplitTraits, SlistNodeAlgorithms> rollback
               ( this->priv_bucket(split_idx), old_bucket, this->priv_split_traits());
            Siterator before_i(old_bucket.get_node_ptr());
            Siterator i(before_i); ++i;
            Siterator end_sit = LINEAR_BUCKETS ? Siterator() : before_i;
            for( ; i != end_sit; i = before_i, ++i){
               const ValueType &v = this->priv_value_from_siterator(i);
               const std::size_t hash_value = this->priv_stored_or_compute_hash(v, StoreHashT());
               const std::size_t new_n = this->priv_hash_to_nbucket(hash_value);
               Siterator last = i;
               (priv_go_to_last_in_group)(last, OptimizeMultikeyT());
               if(new_n == bucket_to_rehash){
                  before_i = last;
               }
               else{
                  BucketType &new_b = this->priv_bucket(new_n);
                  SlistNodeAlgorithms::transfer_after(new_b.get_node_ptr(), before_i.pointed_node(), last.pointed_node());
               }
            }
            rollback.release();
            this->priv_erasure_update_cache();
         }
      }
      else if((ret = split_idx > bucket_cnt/2u)){   //!grow
         const std::size_t target_bucket_num = split_idx - 1u - bucket_cnt/2u;
         BucketType &target_bucket = this->priv_bucket(target_bucket_num);
         BucketType &source_bucket = this->priv_bucket(split_idx-1u);
         SlistNodeAlgorithms::transfer_after(target_bucket.get_node_ptr(), source_bucket.get_node_ptr());
         this->dec_split_count();
         this->priv_insertion_update_cache(target_bucket_num);
      }
      return ret;
   }

   //! <b>Effects</b>: If new_bucket_traits.bucket_count() is not
   //!   this->bucket_count()/2 or this->bucket_count()*2, or
   //!   this->split_bucket() != new_bucket_traits.bucket_count() returns false
   //!   and does nothing.
   //!
   //!   Otherwise, copy assigns new_bucket_traits to the internal bucket_traits
   //!   and transfers all the objects from old buckets to the new ones.
   //!
   //! <b>Complexity</b>: Linear to size().
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Note</b>: this method is only available if incremental<true> option is activated.
   bool incremental_rehash(const BucketTraits &new_bucket_traits) BOOST_NOEXCEPT
   {
      //This function is only available for containers with incremental hashing
      BOOST_INTRUSIVE_STATIC_ASSERT(( INCREMENTAL && POWER_2_BUCKETS ));
      const BucketPtr new_buckets = new_bucket_traits.bucket_begin();
      const SizeType new_bucket_count_stdszt = static_cast<TSizeType>(new_bucket_traits.bucket_count() - BUCKET_OVERHEAD);
      BOOST_INTRUSIVE_INVARIANT_ASSERT(sizeof(SizeType) >= sizeof(std::size_t) || new_bucket_count_stdszt <= SizeType(-1));
      SizeType new_bucket_count = static_cast<SizeType>(new_bucket_count_stdszt);
      const SizeType old_bucket_count = static_cast<SizeType>(this->priv_usable_bucket_count());
      const SizeType split_idx = this->split_count();

      //Test new bucket size is consistent with internal bucket size and split count
      if(new_bucket_count/2 == old_bucket_count){
         if(!(split_idx >= old_bucket_count))
            return false;
      }
      else if(new_bucket_count == old_bucket_count/2){
         if(!(split_idx <= new_bucket_count))
            return false;
      }
      else{
         return false;
      }

      const SizeType ini_n = (SizeType)this->priv_get_cache_bucket_num();
      const BucketPtr old_buckets = this->priv_bucket_pointer();


      this->priv_unset_sentinel_bucket();
      this->priv_initialize_new_buckets(old_buckets, old_bucket_count, new_buckets, new_bucket_count);
      if (&new_bucket_traits != &this->priv_bucket_traits())
         this->priv_bucket_traits() = new_bucket_traits;

      if(old_buckets != new_buckets){
         for(SizeType n = ini_n; n < split_idx; ++n){
            SlistNodePtr new_bucket_nodeptr = new_bucket_traits.bucket_begin()[DifferenceType(n)].get_node_ptr();
            SlistNodePtr old_bucket_node_ptr = old_buckets[DifferenceType(n)].get_node_ptr();
            SlistNodeAlgorithms::transfer_after(new_bucket_nodeptr, old_bucket_node_ptr);
         }
         //Reset cache to safe position
         this->priv_set_cache_bucket_num(ini_n);
      }

      this->priv_set_sentinel_bucket();
      return true;
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! <b>Requires</b>: incremental<> option must be set
   //!
   //! <b>Effects</b>: returns the current split count
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: Nothing
   size_type split_count() const BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is bigger or equal than n. This suggestion can be
   //!   used to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the
   //!   higher possible value is returned.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static size_type suggested_upper_bucket_count(size_type n) BOOST_NOEXCEPT;

   //! <b>Effects</b>: Returns the nearest new bucket count optimized for
   //!   the container that is smaller or equal than n. This suggestion can be
   //!   used to create bucket arrays with a size that will usually improve
   //!   container's performance. If such value does not exist, the
   //!   lowest possible value is returned.
   //!
   //! <b>Complexity</b>: Amortized constant time.
   //!
   //! <b>Throws</b>: Nothing.
   static size_type suggested_lower_bucket_count(size_type n) BOOST_NOEXCEPT;
   #endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)


   friend bool operator==(const HashtableImpl &x, const HashtableImpl &y)
   {
      //Taken from N3068
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
      if(x.size() != y.size()){
         return false;
      }

      if (boost::intrusive::iterator_udistance(x.begin(), x.end()) != x.size())
         return false;
         
      for (ConstIterator ix = x.cbegin(), ex = x.cend(); ix != ex; ++ix){
         std::pair<ConstIterator, ConstIterator> eqx(x.equal_range(KeyOfValue()(*ix))),
                                                   eqy(y.equal_range(KeyOfValue()(*ix)));
         if (boost::intrusive::iterator_distance(eqx.first, eqx.second) !=
             boost::intrusive::iterator_distance(eqy.first, eqy.second) ||
               !(priv_algo_is_permutation)(eqx.first, eqx.second, eqy.first)      ){
            return false;
         }
         ix = eqx.second;
      }
      return true;
   }

   friend bool operator!=(const HashtableImpl &x, const HashtableImpl &y)
   {  return !(x == y); }

   friend bool operator<(const HashtableImpl &x, const HashtableImpl &y)
   {  return ::boost::intrusive::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   friend bool operator>(const HashtableImpl &x, const HashtableImpl &y)
   {  return y < x;  }

   friend bool operator<=(const HashtableImpl &x, const HashtableImpl &y)
   {  return !(y < x);  }

   friend bool operator>=(const HashtableImpl &x, const HashtableImpl &y)
   {  return !(x < y);  }

   /// @cond
   inline void check() const {}
   private:

   static void priv_initialize_new_buckets
      ( BucketPtr old_buckets, SizeType old_bucket_count
      , BucketPtr new_buckets, SizeType new_bucket_count)
   {
      //Initialize new buckets
      const bool same_buffer = old_buckets == new_buckets;
      if (same_buffer && new_bucket_count <= old_bucket_count) {
         //Nothing to do here
      }
      else {
         BucketPtr p;
         SizeType c;

         if (same_buffer) {
            p = old_buckets + std::ptrdiff_t(old_bucket_count);
            c = SizeType(new_bucket_count - old_bucket_count);
         }
         else {
            p = new_buckets;
            c = new_bucket_count;
         }
         InternalType::priv_init_buckets(p, c);
      }
   }

   void priv_rehash_impl(const BucketTraits &new_bucket_traits, bool do_full_rehash)
   {
      const std::size_t nbc             = new_bucket_traits.bucket_count() - BUCKET_OVERHEAD;
      BOOST_INTRUSIVE_INVARIANT_ASSERT(sizeof(TSizeType) >= sizeof(std::size_t) || nbc <= TSizeType(-1));

      const BucketPtr new_buckets      = new_bucket_traits.bucket_begin();
      const SizeType  new_bucket_count = static_cast<TSizeType>(nbc);
      const BucketPtr old_buckets      = this->priv_bucket_pointer();
      const SizeType  old_bucket_count = this->bucket_count();

      //Check power of two bucket array if the option is activated
      BOOST_INTRUSIVE_INVARIANT_ASSERT
         (!POWER_2_BUCKETS || (0 == (new_bucket_count & (new_bucket_count-1u))));

      const bool same_buffer = old_buckets == new_buckets;
      //If the new bucket length is a common factor
      //of the old one we can avoid hash calculations.
      const bool fast_shrink = (!do_full_rehash) && (!INCREMENTAL) && (old_bucket_count >= new_bucket_count) &&
         (POWER_2_BUCKETS || (old_bucket_count % new_bucket_count) == 0);
      //If we are shrinking the same bucket array and it's
      //is a fast shrink, just rehash the last nodes
      SizeType new_first_bucket_num = new_bucket_count;
      SizeType old_bucket_cache = (SizeType)this->priv_get_cache_bucket_num();
      if(same_buffer && fast_shrink && (old_bucket_cache < new_bucket_count)){
         new_first_bucket_num = old_bucket_cache;
         old_bucket_cache = new_bucket_count;
      }

      if (!do_full_rehash)
         this->priv_initialize_new_buckets(old_buckets, old_bucket_count, new_buckets, new_bucket_count);

      //Anti-exception stuff: they destroy the elements if something goes wrong.
      //If the source and destination buckets are the same, the second rollback function
      //is harmless, because all elements have been already unlinked and destroyed

      typedef typename InternalType::template typeof_node_disposer<detail::NullDisposer>::type NodeDisposer;
      typedef ExceptionBucketDisposer<BucketType, SlistNodeAlgorithms, NodeDisposer, SizeType> ArrayDisposer;
      NodeDisposer nd(this->make_node_disposer(detail::NullDisposer()));
      ArrayDisposer rollback1(new_buckets[0], nd, new_bucket_count);
      ArrayDisposer rollback2(old_buckets[0], nd, old_bucket_count);

      //Put size in a safe value for rollback exception
      SizeType const size_backup = this->priv_size_count();
      this->priv_size_count(0);
      //Put cache to safe position
      this->priv_init_cache();
      this->priv_unset_sentinel_bucket();

      const SizeType split = this->rehash_split_from_bucket_count(new_bucket_count);

      //Iterate through nodes
      for(SizeType n = old_bucket_cache; n < old_bucket_count; ++n){
         BucketType &old_bucket = old_buckets[DifferenceType(n)];
         if(!fast_shrink){
            Siterator before_i(old_bucket.get_node_ptr());
            Siterator i(before_i); ++i;
            Siterator end_sit(this->sit_end(old_bucket));
            for( //
               ; i != end_sit
               ; i = before_i, ++i){

               //First obtain hash value (and store it if do_full_rehash)
               std::size_t hash_value;
               if(do_full_rehash){
                  ValueType &v = this->priv_value_from_siterator(i);
                  hash_value = this->priv_hasher()(KeyOfValue()(v));
                  NodeFunctionsT::store_hash(this->priv_value_to_node_ptr(v), hash_value, StoreHashT());
               }
               else{
                  const ValueType &v = this->priv_value_from_siterator(i);
                  hash_value = this->priv_stored_or_compute_hash(v, StoreHashT());
               }

               //Now calculate the new bucket position
               const SizeType new_n = (SizeType)hash_to_bucket_split<POWER_2_BUCKETS, INCREMENTAL>
                  (hash_value, new_bucket_count, split, FastmodBucketsT());

               //Update first used bucket cache
               if(CACHE_BEGIN && new_n < new_first_bucket_num)
                  new_first_bucket_num = new_n;

               //If the target bucket is new, transfer the whole group
               Siterator last = i;
               (priv_go_to_last_in_group)(i, OptimizeMultikeyT());

               if(same_buffer && new_n == n){
                  before_i = last;
               }
               else{
                  BucketType &new_b = new_buckets[DifferenceType(new_n)];
                  SlistNodeAlgorithms::transfer_after(new_b.get_node_ptr(), before_i.pointed_node(), last.pointed_node());
               }
            }
         }
         else{
            const SizeType new_n = (SizeType)hash_to_bucket_split<POWER_2_BUCKETS, INCREMENTAL>
                                       (n, new_bucket_count, split, FastmodBucketsT());
            if(CACHE_BEGIN && new_n < new_first_bucket_num)
               new_first_bucket_num = new_n;
            BucketType &new_b = new_buckets[DifferenceType(new_n)];
            Siterator last = this->priv_get_last(old_bucket, OptimizeMultikeyT());
            SlistNodeAlgorithms::transfer_after(new_b.get_node_ptr(), old_bucket.get_node_ptr(), last.pointed_node());
         }
      }

      this->priv_size_count(size_backup);
      this->split_count(split);
      if(&new_bucket_traits != &this->priv_bucket_traits())
         this->priv_bucket_traits() = new_bucket_traits;
      this->priv_set_sentinel_bucket();
      this->priv_set_cache_bucket_num(new_first_bucket_num);
      rollback1.release();
      rollback2.release();
   }

   template <class MaybeConstHashtableImpl, class Cloner, class Disposer>
   void priv_clone_from(MaybeConstHashtableImpl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      if(!CONSTANT_TIME_SIZE || !src.empty()){
         const SizeType src_bucket_count = src.bucket_count();
         const SizeType dst_bucket_count = this->bucket_count();
         //Check power of two bucket array if the option is activated
         BOOST_INTRUSIVE_INVARIANT_ASSERT
            (!POWER_2_BUCKETS || (0 == (src_bucket_count & (src_bucket_count-1))));
         BOOST_INTRUSIVE_INVARIANT_ASSERT
            (!POWER_2_BUCKETS || (0 == (dst_bucket_count & (dst_bucket_count-1))));
         //If src bucket count is bigger or equal, structural copy is possible
         const bool structural_copy = (!INCREMENTAL) && (src_bucket_count >= dst_bucket_count) &&
            (POWER_2_BUCKETS || (src_bucket_count % dst_bucket_count) == 0);
         if(structural_copy){
            this->priv_structural_clone_from(src, cloner, disposer);
         }
         else{
            //Unlike previous cloning algorithm, this can throw
            //if cloner, hasher or comparison functor throw
            typedef typename detail::if_c< detail::is_const<MaybeConstHashtableImpl>::value
                                         , typename MaybeConstHashtableImpl::const_iterator
                                         , typename MaybeConstHashtableImpl::iterator
                                         >::type CloneIterator;
            CloneIterator b(src.begin()), e(src.end());
            detail::ExceptionDisposer<HashtableImpl, Disposer> rollback(*this, disposer);
            for(; b != e; ++b){
               //No need to check for duplicates and insert it in the first position
               //as this is an unordered container. So use minimal insertion code
               std::size_t const hash_to_store = this->priv_stored_or_compute_hash(*b, StoreHashT());
               SizeType const bucket_number = this->priv_hash_to_nbucket(hash_to_store);
               typedef typename detail::if_c
                  <detail::is_const<MaybeConstHashtableImpl>::value, ConstReference, Reference>::type ReferenceType;
               ReferenceType r = *b;
               this->priv_clone_front_in_bucket<ReferenceType>(bucket_number, r, hash_to_store, cloner);
            }
            rollback.release();
         }
      }
   }

   template<class ValueReference, class Cloner>
   void priv_clone_front_in_bucket( SizeType const bucket_number
                                  , typename detail::identity<ValueReference>::type src_ref
                                  , std::size_t const hash_to_store, Cloner cloner)
   {
      //No need to check for duplicates and insert it in the first position
      //as this is an unordered container. So use minimal insertion code
      BucketType &cur_bucket = this->priv_bucket(bucket_number);
      Siterator const prev(cur_bucket.get_node_ptr());
      //Just check if the cloned node is equal to the first inserted value in the new bucket
      //as equal src values were contiguous and they should be already inserted in the
      //destination bucket.
      bool const next_is_in_group = OPTIMIZE_MULTIKEY && !this->priv_bucket_empty(bucket_number) &&
         this->priv_equal()( KeyOfValue()(src_ref)
                           , KeyOfValue()(this->priv_value_from_siterator(++Siterator(prev))));
      this->priv_insert_equal_after_find(*cloner(src_ref), bucket_number, hash_to_store, prev, next_is_in_group);
   }

   template <class MaybeConstHashtableImpl, class Cloner, class Disposer>
   void priv_structural_clone_from(MaybeConstHashtableImpl &src, Cloner cloner, Disposer disposer)
   {
      //First clone the first ones
      const SizeType src_bucket_count = src.bucket_count();
      const SizeType dst_bucket_count = this->bucket_count();
      SizeType constructed = 0;
      typedef typename InternalType::template typeof_node_disposer<Disposer>::type NodeDisposer;
      NodeDisposer node_disp(disposer, &this->priv_value_traits());

      ExceptionBucketDisposer<BucketType, SlistNodeAlgorithms, NodeDisposer, SizeType>
         rollback(this->priv_bucket(0), node_disp, constructed);
      //Now insert the remaining ones using the modulo trick
      for( //"constructed" already initialized
         ; constructed < src_bucket_count
         ; ++constructed){

         const SizeType new_n = (SizeType)hash_to_bucket_split<POWER_2_BUCKETS, INCREMENTAL>
            (constructed, dst_bucket_count, this->split_count(), FastmodBucketsT());
         BucketType &src_b = src.priv_bucket(constructed);
         for( Siterator b(this->priv_bucket_lbegin(src_b)), e(this->priv_bucket_lend(src_b)); b != e; ++b){
            typedef typename detail::if_c
               <detail::is_const<MaybeConstHashtableImpl>::value, ConstReference, Reference>::type ReferenceType;
            ReferenceType r = this->priv_value_from_siterator(b);
            this->priv_clone_front_in_bucket<ReferenceType>
               (new_n, r, this->priv_stored_hash(b, StoreHashT()), cloner);
         }
      }
      this->priv_hasher() = src.priv_hasher();
      this->priv_equal()  = src.priv_equal();
      rollback.release();
      this->priv_size_count(src.priv_size_count());
      this->split_count(dst_bucket_count);
      this->priv_set_cache_bucket_num(0u);
      this->priv_erasure_update_cache();
   }

   Iterator priv_insert_equal_after_find(Reference value, SizeType bucket_num, std::size_t hash_value, Siterator prev, bool const next_is_in_group)
   {
      //Now store hash if needed
      NodePtr n = this->priv_value_to_node_ptr(value);
      NodeFunctionsT::store_hash(n, hash_value, StoreHashT());
      //Checks for some modes
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || SlistNodeAlgorithms::unique(n));
      //Shortcut to optimize_multikey cases
      GroupFunctionsT::insert_in_group
         ( next_is_in_group ? dcast_bucket_ptr<Node>((++Siterator(prev)).pointed_node()) : n
         , n, OptimizeMultikeyT());
      //Update cache and increment size if needed
      this->priv_insertion_update_cache(bucket_num);
      this->priv_size_inc();
      SlistNodeAlgorithms::link_after(prev.pointed_node(), n);
      return this->build_iterator(Siterator(n), this->priv_bucket_ptr(bucket_num));
   }

   template<class KeyType, class KeyHasher, class KeyEqual>
   Siterator priv_find  //In case it is not found previt is priv_end_sit()
      ( const KeyType &key,  KeyHasher hash_func
      , KeyEqual equal_func, SizeType &bucket_number, std::size_t &h, Siterator &previt) const
   {
      h = hash_func(key);

      bucket_number = this->priv_hash_to_nbucket(h);
      BucketType& b = this->priv_bucket(bucket_number);
      Siterator prev = this->sit_bbegin(b);
      Siterator it = prev;
      Siterator const endit = this->sit_end(b);

      while (++it != endit) {
         if (this->priv_is_value_equal_to_key
               (this->priv_value_from_siterator(it), h, key, equal_func, CompareHashT())) {
            previt = prev;
            return it;
         }
         (priv_go_to_last_in_group)(it, OptimizeMultikeyT());
         prev = it;
      }
      previt = b.get_node_ptr();
      return this->priv_end_sit();
   }


   template<class KeyType, class KeyEqual>
   Siterator priv_find_in_bucket  //In case it is not found previt is priv_end_sit()
      (BucketType &b, const KeyType& key, KeyEqual equal_func, const std::size_t h) const
   {
      Siterator it(this->sit_begin(b));
      Siterator const endit(this->sit_end(b));

      for (; it != endit; (priv_go_to_last_in_group)(it, OptimizeMultikeyT()), ++it) {
         if (BOOST_LIKELY(this->priv_is_value_equal_to_key
               (this->priv_value_from_siterator(it), h, key, equal_func, CompareHashT()))) {
            return it;
         }
      }
      return this->priv_end_sit();
   }

   template<class KeyType, class KeyEqual>
   inline bool priv_is_value_equal_to_key
      (const ValueType &v, const std::size_t h, const KeyType &key, KeyEqual equal_func, detail::true_) const //compare_hash
   {  return this->priv_stored_or_compute_hash(v, StoreHashT()) == h && equal_func(key, KeyOfValue()(v));  }

   template<class KeyType, class KeyEqual>
   inline bool priv_is_value_equal_to_key
      (const ValueType& v, const std::size_t , const KeyType& key, KeyEqual equal_func, detail::false_) const //compare_hash
   {  return equal_func(key, KeyOfValue()(v));   }

   //return previous iterator to the next equal range group in case
   inline static void priv_go_to_last_in_group
      (Siterator &it_first_in_group, detail::true_) BOOST_NOEXCEPT  //optimize_multikey
   {
      it_first_in_group =
         (GroupFunctionsT::get_last_in_group
            (dcast_bucket_ptr<Node>(it_first_in_group.pointed_node()), OptimizeMultikeyT()));
   }

   //return previous iterator to the next equal range group in case
   inline static void priv_go_to_last_in_group        //!optimize_multikey
      (Siterator /*&it_first_in_group*/, detail::false_) BOOST_NOEXCEPT
   { }

   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<Siterator, Siterator> priv_local_equal_range
      ( const KeyType &key
      , KeyHasher hash_func
      , KeyEqual equal_func
      , SizeType &found_bucket
      , SizeType &cnt) const
   {
      std::size_t internal_cnt = 0;
      //Let's see if the element is present
      
      Siterator prev;
      SizeType n_bucket;
      std::size_t h;
      std::pair<Siterator, Siterator> to_return
         ( this->priv_find(key, hash_func, equal_func, n_bucket, h, prev)
         , this->priv_end_sit());

      if(to_return.first != to_return.second){
         found_bucket = n_bucket;
         //If it's present, find the first that it's not equal in
         //the same bucket
         Siterator it = to_return.first;
         Siterator const bend = this->priv_bucket_lend(n_bucket);
         BOOST_IF_CONSTEXPR(OPTIMIZE_MULTIKEY){
            Siterator past_last_in_group_it = it;
            (priv_go_to_last_in_group)(past_last_in_group_it, OptimizeMultikeyT());
            ++past_last_in_group_it;
            internal_cnt += boost::intrusive::iterator_udistance(++it, past_last_in_group_it) + 1u;
            if (past_last_in_group_it != bend)
               to_return.second = past_last_in_group_it;
         }
         else{
            do {
               ++internal_cnt;   //At least one is found
               ++it;
            } while(it != bend &&
                     this->priv_is_value_equal_to_key
                        (this->priv_value_from_siterator(it), h, key, equal_func, CompareHashT()));
            if (it != bend)
               to_return.second = it;
         }
      }
      cnt = SizeType(internal_cnt);
      return to_return;
   }

   struct PrivEqualRangeResult
   {
      Siterator m_first;
      Siterator m_second;
      BucketPtr m_bucket_first;
      BucketPtr m_bucket_second;
   };

   template<class KeyType, class KeyHasher, class KeyEqual>
   PrivEqualRangeResult priv_equal_range
      ( const KeyType &key
      , KeyHasher hash_func
      , KeyEqual equal_func) const
   {
      SizeType n_bucket;
      SizeType cnt;

      //Let's see if the element is present
      const std::pair<Siterator, Siterator> to_return
         (this->priv_local_equal_range(key, hash_func, equal_func, n_bucket, cnt));
      PrivEqualRangeResult r;
      r.m_first = to_return.first;
      r.m_second = to_return.second;

      //If not, find the next element as ".second" if ".second" local iterator
      //is not pointing to an element.
      if(to_return.first == to_return.second) {
         r.m_bucket_first = r.m_bucket_second = this->priv_invalid_bucket_ptr();
      }
      else if (to_return.second != this->priv_end_sit()) {
         r.m_bucket_first = this->priv_bucket_ptr(n_bucket);
      }
      else{
         r.m_bucket_first = this->priv_bucket_ptr(n_bucket);
         const SizeType max_bucket = this->bucket_count();
         do{
            ++n_bucket;
         } while (n_bucket != max_bucket && this->priv_bucket_empty(n_bucket));

         if (n_bucket == max_bucket){
            r.m_bucket_second = this->priv_invalid_bucket_ptr();
         }
         else{
            r.m_bucket_second = this->priv_bucket_ptr(n_bucket);
            r.m_second = Siterator(r.m_bucket_second->begin_ptr());
         }
      }

      return r;
   }

   inline SizeType priv_get_bucket_num(ConstIterator it) BOOST_NOEXCEPT
   {  return this->priv_get_bucket_num(it, LinearBucketsT());  }

   inline SizeType priv_get_bucket_num(ConstIterator it, detail::true_) BOOST_NOEXCEPT //linear
   {  return SizeType(it.get_bucket_ptr() - this->priv_bucket_pointer());   }

   inline SizeType priv_get_bucket_num(ConstIterator it, detail::false_) BOOST_NOEXCEPT //!linear
   {  return this->priv_get_bucket_num_hash_dispatch(it.slist_it(), StoreHashT());  }

   inline SizeType priv_get_bucket_num_hash_dispatch(Siterator it, detail::true_) BOOST_NOEXCEPT    //store_hash
   {  return (SizeType)this->priv_hash_to_nbucket(this->priv_stored_hash(it, StoreHashT()));  }

   SizeType priv_get_bucket_num_hash_dispatch(Siterator it, detail::false_) BOOST_NOEXCEPT   //NO store_hash
   {
      const BucketType &f = this->priv_bucket(0u);
      SlistNodePtr bb = GroupFunctionsT::get_bucket_before_begin
         ( this->priv_bucket_lbbegin(0u).pointed_node()
         , this->priv_bucket_lbbegin(this->priv_usable_bucket_count() - 1u).pointed_node()
         , it.pointed_node()
         , OptimizeMultikeyT());

      //Now get the bucket_impl from the iterator
      const BucketType &b = static_cast<const BucketType&>(*bb);
      //Now just calculate the index b has in the bucket array
      return static_cast<SizeType>(&b - &f);
   }


   inline BucketPtr priv_get_bucket_ptr(ConstIterator it) BOOST_NOEXCEPT
   {  return this->priv_get_bucket_ptr(it, LinearBucketsT());  }

   inline BucketPtr priv_get_bucket_ptr(ConstIterator it, detail::true_) BOOST_NOEXCEPT //linear
   {  return it.get_bucket_ptr();   }

   inline BucketPtr priv_get_bucket_ptr(ConstIterator it, detail::false_) BOOST_NOEXCEPT //!linear
   {  return this->priv_bucket_ptr(this->priv_get_bucket_num_hash_dispatch(it.slist_it(), StoreHashT()));  }

   /// @endcond
};

/// @cond
template < class T
         , class PackedOptions
         >
struct MakeBucketTraits
{
   //Real value traits must be calculated from options
   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef typename PackedOptions::bucket_traits            SpecifiedBucketTraits;

   //Real bucket traits must be calculated from options and calculated value_traits
   typedef BucketTraitsImpl
      < typename UnorderedBucketPtrImpl
         <ValueTraits>::type
      , std::size_t>                                        BucketTraitsT;

   typedef typename
      detail::if_c< detail::is_same
                     < SpecifiedBucketTraits
                     , default_bucket_traits
                     >::value
                  , BucketTraitsT
                  , SpecifiedBucketTraits
                  >::Type                                Type;
};
/// @endcond

//! Helper metafunction to define a \c hashtable that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void, class O8 = void
                , class O9 = void, class O10= void
                , class O11= void
                >
#endif
struct MakeHashtable
{
   /// @cond
   typedef typename PackOptions
      < HashtableDefaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10, O11
         #else
         Options...
         #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef typename MakeBucketTraits
            <T, PackedOptions>::type BucketTraits;

   typedef HashtableImpl
      < ValueTraits
      , typename PackedOptions::key_of_value
      , typename PackedOptions::hash
      , typename PackedOptions::equal
      , BucketTraits
      , typename PackedOptions::size_type
      ,  (std::size_t(false)*HashBoolFlags::UNIQUE_KEYS_POS)
        |(std::size_t(PackedOptions::constant_time_size)*HashBoolFlags::CONSTANT_TIME_SIZE_POS)
        |(std::size_t(PackedOptions::power_2_buckets)*HashBoolFlags::POWER_2_BUCKETS_POS)
        |(std::size_t(PackedOptions::cache_begin)*HashBoolFlags::CACHE_BEGIN_POS)
        |(std::size_t(PackedOptions::compare_hash)*HashBoolFlags::COMPARE_HASH_POS)
        |(std::size_t(PackedOptions::incremental)*HashBoolFlags::INCREMENTAL_POS)
        |(std::size_t(PackedOptions::linear_buckets)*HashBoolFlags::LINEAR_BUCKETS_POS)
        |(std::size_t(PackedOptions::fastmod_buckets)*HashBoolFlags::FASTMOD_BUCKETS_POS)
      > ImplementationDefined;

   /// @endcond
   typedef ImplementationDefined Type;
};

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

#if defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10>
#endif
class Hashtable
   :  public MakeHashtable<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
         >::type
{
   typedef typename MakeHashtable<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Hashtable)

   public:
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::BucketPtr         BucketPtr;
   typedef typename Base::SizeType          SizeType;
   typedef typename Base::Hasher             Hasher;
   typedef typename Base::BucketTraits      BucketTraits;
   typedef typename Base::KeyEqual          KeyEqual;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline explicit Hashtable ( const BucketTraits &b_traits
             , const Hasher & hash_func = Hasher()
             , const KeyEqual &equal_func = KeyEqual()
             , const ValueTraits &v_traits = ValueTraits())
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   inline Hashtable(BOOST_RV_REF(Hashtable) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Hashtable& operator=(BOOST_RV_REF(Hashtable) x)
   {  return static_cast<Hashtable&>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Hashtable &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Hashtable) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_HASHTABLE_HPP
