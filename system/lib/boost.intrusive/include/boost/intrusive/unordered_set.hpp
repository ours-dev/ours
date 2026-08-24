/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Olaf Krzikalla 2004-2006.
// (C) Copyright Ion Gaztanaga  2006-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_UNORDERED_SET_HPP
#define BOOST_INTRUSIVE_UNORDERED_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/hashtable.hpp>
#include <boost/move/utility_core.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

//! The class template unordered_set is an intrusive container, that mimics most of
//! the interface of std::tr1::unordered_set as described in the C++ TR1.
//!
//! unordered_set is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: unordered_set needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! unordered_set more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, \c power_2_buckets<> and \c cache_begin<>.
//!
//! unordered_set only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size unordered_sets because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant-time size unordered_sets are mainly provided to support auto-unlink hooks.
//!
//! unordered_set, unlike std::unordered_set, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehasing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TSizeType, class TBucketTraits, std::size_t BoolFlags>
#endif
class UnorderedSetImpl
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public HashtableImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags|HashBoolFlags::UNIQUE_KEYS_POS>
   #endif
{
   /// @cond
   private:
   typedef HashtableImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags|HashBoolFlags::UNIQUE_KEYS_POS> TableType;

   template<class Iterator, class MaybeConstThis, class KeyType, class KeyHasher, class KeyEqual>
   static std::pair<Iterator,Iterator> priv_equal_range(MaybeConstThis &c, const KeyType& key, KeyHasher hash_func, KeyEqual equal_func)
   {
      Iterator const it = c.find(key, hash_func, equal_func);
      std::pair<Iterator,Iterator> ret(it, it);      
      if(it != c.end())
         ++ret.second;
      return ret;
   }

   //! This class is
   //! movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(UnorderedSetImpl)

   typedef TableType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType                  ValueType;
   typedef typename ImplementationDefined::KeyType                    KeyType;
   typedef typename ImplementationDefined::KeyOfValue                KeyOfValue;
   typedef typename ImplementationDefined::ValueTraits                ValueTraits;
   typedef typename ImplementationDefined::BucketTraits               BucketTraits;
   typedef typename ImplementationDefined::Pointer                     Pointer;
   typedef typename ImplementationDefined::ConstPointer               ConstPointer;
   typedef typename ImplementationDefined::Reference                   Reference;
   typedef typename ImplementationDefined::ConstReference             ConstReference;
   typedef typename ImplementationDefined::DifferenceType             DifferenceType;
   typedef typename ImplementationDefined::SizeType                   SizeType;
   typedef typename ImplementationDefined::KeyEqual                   KeyEqual;
   typedef typename ImplementationDefined::Hasher                      Hasher;
   typedef typename ImplementationDefined::BucketType                 BucketType;
   typedef typename ImplementationDefined::BucketPtr                  BucketPtr;
   typedef typename ImplementationDefined::Iterator                    Iterator;
   typedef typename ImplementationDefined::ConstIterator              ConstIterator;
   typedef typename ImplementationDefined::InsertCommitData          InsertCommitData;
   typedef typename ImplementationDefined::LocalIterator              LocalIterator;
   typedef typename ImplementationDefined::ConstLocalIterator        ConstLocalIterator;
   typedef typename ImplementationDefined::NodeTraits                 NodeTraits;
   typedef typename ImplementationDefined::Node                        Node;
   typedef typename ImplementationDefined::NodePtr                    NodePtr;
   typedef typename ImplementationDefined::ConstNodePtr              ConstNodePtr;

   public:

   //! @copydoc ::boost::intrusive::hashtable::hashtable(const bucket_traits &,const hasher &,const key_equal &,const value_traits &)
   inline explicit UnorderedSetImpl( const BucketTraits &b_traits
                              , const Hasher & hash_func = Hasher()
                              , const KeyEqual &equal_func = KeyEqual()
                              , const ValueTraits &v_traits = ValueTraits())
      :  TableType(b_traits, hash_func, equal_func, v_traits)
   {}

   //! @copydoc ::boost::intrusive::hashtable::hashtable(bool,Iterator,Iterator,const bucket_traits &,const hasher &,const key_equal &,const value_traits &)
   template<class Iterator>
   inline UnorderedSetImpl( Iterator b
                     , Iterator e
                     , const BucketTraits &b_traits
                     , const Hasher & hash_func = Hasher()
                     , const KeyEqual &equal_func = KeyEqual()
                     , const ValueTraits &v_traits = ValueTraits())
      :  TableType(true, b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   //! @copydoc ::boost::intrusive::hashtable::hashtable(hashtable&&)
   inline UnorderedSetImpl(BOOST_RV_REF(UnorderedSetImpl) x)
      :  TableType(BOOST_MOVE_BASE(TableType, x))
   {}

   //! @copydoc ::boost::intrusive::hashtable::operator=(hashtable&&)
   inline UnorderedSetImpl& operator=(BOOST_RV_REF(UnorderedSetImpl) x)
   {  return static_cast<UnorderedSetImpl&>(TableType::operator=(BOOST_MOVE_BASE(TableType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::hashtable::~hashtable()
   ~UnorderedSetImpl();

   //! @copydoc ::boost::intrusive::hashtable::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::hash_function()const
   hasher hash_function() const;

   //! @copydoc ::boost::intrusive::hashtable::key_eq()const
   key_equal key_eq() const;

   //! @copydoc ::boost::intrusive::hashtable::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::hashtable
   void swap(UnorderedSetImpl& other);

   //! @copydoc ::boost::intrusive::hashtable::clone_from(const hashtable&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const UnorderedSetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TableType::clone_from;

   #endif //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::hashtable::clone_from(hashtable&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(UnorderedSetImpl) src, Cloner cloner, Disposer disposer)
   {  TableType::clone_from(BOOST_MOVE_BASE(TableType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique(reference)
   inline std::pair<Iterator, bool> insert(Reference value)
   {  return TableType::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique(Iterator,Iterator)
   template<class Iterator>
   inline void insert(Iterator b, Iterator e)
   {  TableType::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique_check(const key_type&,insert_commit_data&)
   inline std::pair<Iterator, bool> insert_check(const KeyType &key, InsertCommitData &commit_data)
   {  return TableType::insert_unique_check(key, commit_data); }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique_check(const KeyType&,KeyHasher,KeyEqual,insert_commit_data&)
   template<class KeyType, class KeyHasher, class KeyEqual>
   inline std::pair<Iterator, bool> insert_check
      (const KeyType &key, KeyHasher hash_func, KeyEqual key_value_equal, InsertCommitData &commit_data)
   {  return TableType::insert_unique_check(key, hash_func, key_value_equal, commit_data); }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique_commit
   inline Iterator insert_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return TableType::insert_unique_commit(value, commit_data); }

   //! @copydoc ::boost::intrusive::hashtable::insert_unique_fast_commit
   inline Iterator insert_fast_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return TableType::insert_unique_fast_commit(value, commit_data); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::hashtable::erase(const_iterator)
   void erase(const_iterator i);

   //! @copydoc ::boost::intrusive::hashtable::erase(const_iterator,const_iterator)
   void erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::hashtable::erase(const KeyType&,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   size_type erase(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func);

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   BOOST_INTRUSIVE_DOC1ST(void
      , typename detail::disable_if_convertible<Disposer BOOST_INTRUSIVE_I const_iterator>::type)
      erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   void erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const key_type &,Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const KeyType&,KeyHasher,KeyEqual,Disposer)
   template<class KeyType, class KeyHasher, class KeyEqual, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func, Disposer disposer);

   //! @copydoc ::boost::intrusive::hashtable::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::count(const key_type &)const
   size_type count(const key_type &key) const;

   //! @copydoc ::boost::intrusive::hashtable::count(const KeyType&,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   size_type count(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const;

   //! @copydoc ::boost::intrusive::hashtable::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::hashtable::find(const KeyType &,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   iterator find(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func);

   //! @copydoc ::boost::intrusive::hashtable::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::hashtable::find(const KeyType &,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   const_iterator find(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const;
   #endif

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const key_type&)
   std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->equal_range(key, this->hash_function(), this->key_eq());  }

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const KeyType &,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<Iterator,Iterator> equal_range(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func)
   {  return this->priv_equal_range<Iterator>(*this, key, hash_func, equal_func); }

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const key_type&)const
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->equal_range(key, this->hash_function(), this->key_eq());  }

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const KeyType &,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const
   {  return this->priv_equal_range<ConstIterator>(*this, key, hash_func, equal_func); }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! @copydoc ::boost::intrusive::hashtable::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::s_local_iterator_to(reference)
   static local_iterator s_local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::s_local_iterator_to(const_reference)
   static const_local_iterator s_local_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::local_iterator_to(reference)
   local_iterator local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::local_iterator_to(const_reference)
   const_local_iterator local_iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket_count
   size_type bucket_count() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket_size
   size_type bucket_size(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket(const key_type&)const
   size_type bucket(const key_type& k) const;

   //! @copydoc ::boost::intrusive::hashtable::bucket(const KeyType&,KeyHasher)const
   template<class KeyType, class KeyHasher>
   size_type bucket(const KeyType& k,  KeyHasher hash_func) const;

   //! @copydoc ::boost::intrusive::hashtable::bucket_pointer
   bucket_ptr bucket_pointer() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin(size_type)
   local_iterator begin(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin(size_type)const
   const_local_iterator begin(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cbegin(size_type)const
   const_local_iterator cbegin(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end(size_type)
   local_iterator end(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end(size_type)const
   const_local_iterator end(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cend(size_type)const
   const_local_iterator cend(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::rehash(const bucket_traits &)
   void rehash(const bucket_traits &new_bucket_traits);

   //! @copydoc ::boost::intrusive::hashtable::full_rehash
   void full_rehash();

   //! @copydoc ::boost::intrusive::hashtable::incremental_rehash(bool)
   bool incremental_rehash(bool grow = true);

   //! @copydoc ::boost::intrusive::hashtable::incremental_rehash(const bucket_traits &)
   bool incremental_rehash(const bucket_traits &new_bucket_traits);

   //! @copydoc ::boost::intrusive::hashtable::split_count
   size_type split_count() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::suggested_upper_bucket_count
   static size_type suggested_upper_bucket_count(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::suggested_lower_bucket_count
   static size_type suggested_lower_bucket_count(size_type n) BOOST_NOEXCEPT;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   friend bool operator==(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {
      BOOST_IF_CONSTEXPR(TableType::constant_time_size)
      if(x.size() != y.size()){
         return false;
      }

      //Find each element of x in y
      for (ConstIterator ix = x.cbegin(), ex = x.cend(), ey = y.cend(); ix != ex; ++ix){
         ConstIterator iy = y.find(KeyOfValue()(*ix));
         if (iy == ey || !(*ix == *iy))
            return false;
      }
      return true;
   }

   friend bool operator!=(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {  return !(x == y); }

   friend bool operator<(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {  return ::boost::intrusive::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   friend bool operator>(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {  return y < x;  }

   friend bool operator<=(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {  return !(y < x);  }

   friend bool operator>=(const UnorderedSetImpl &x, const UnorderedSetImpl &y)
   {  return !(x < y);  }
};

//! Helper metafunction to define an \c unordered_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void, class O8 = void
                , class O9 = void, class O10= void
                , class O11 = void
                >
#endif
struct MakeUnorderedSet
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

   typedef UnorderedSetImpl
      < ValueTraits
      , typename PackedOptions::key_of_value
      , typename PackedOptions::hash
      , typename PackedOptions::equal
      , typename PackedOptions::size_type
      , BucketTraits
      ,  (std::size_t(true)*HashBoolFlags::UNIQUE_KEYS_POS)
      |  (std::size_t(PackedOptions::constant_time_size)*HashBoolFlags::CONSTANT_TIME_SIZE_POS)
      |  (std::size_t(PackedOptions::power_2_buckets)*HashBoolFlags::POWER_2_BUCKETS_POS)
      |  (std::size_t(PackedOptions::cache_begin)*HashBoolFlags::CACHE_BEGIN_POS)
      |  (std::size_t(PackedOptions::compare_hash)*HashBoolFlags::COMPARE_HASH_POS)
      |  (std::size_t(PackedOptions::incremental)*HashBoolFlags::INCREMENTAL_POS)
      |  (std::size_t(PackedOptions::linear_buckets)*HashBoolFlags::LINEAR_BUCKETS_POS)
      |  (std::size_t(PackedOptions::fastmod_buckets)*HashBoolFlags::FASTMOD_BUCKETS_POS)
      > ImplementationDefined;

   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10, class O11>
#else
template<class T, class ...Options>
#endif
class UnorderedSet
   :  public MakeUnorderedSet<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10, O11
         #else
         Options...
         #endif
      >::type
{
   typedef typename MakeUnorderedSet<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10, O11
         #else
         Options...
         #endif
      >::type Base;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(UnorderedSet)

   public:
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::BucketTraits      BucketTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::BucketPtr         BucketPtr;
   typedef typename Base::SizeType          SizeType;
   typedef typename Base::Hasher             Hasher;
   typedef typename Base::KeyEqual          KeyEqual;

   inline
   explicit UnorderedSet  ( const BucketTraits &b_traits
                           , const Hasher & hash_func = Hasher()
                           , const KeyEqual &equal_func = KeyEqual()
                           , const ValueTraits &v_traits = ValueTraits())
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   template<class Iterator>
   inline
   UnorderedSet
                  ( Iterator b, Iterator e
                  , const BucketTraits &b_traits
                  , const Hasher & hash_func = Hasher()
                  , const KeyEqual &equal_func = KeyEqual()
                  , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   inline UnorderedSet(BOOST_RV_REF(UnorderedSet) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline UnorderedSet& operator=(BOOST_RV_REF(UnorderedSet) x)
   {  return static_cast<UnorderedSet&>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const UnorderedSet &src, Cloner cloner, Disposer disposer)
   {  this->Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(UnorderedSet) src, Cloner cloner, Disposer disposer)
   {  this->Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }
};

#endif


//! The class template unordered_multiset is an intrusive container, that mimics most of
//! the interface of std::tr1::unordered_multiset as described in the C++ TR1.
//!
//! unordered_multiset is a semi-intrusive container: each object to be stored in the
//! container must contain a proper hook, but the container also needs
//! additional auxiliary memory to work: unordered_multiset needs a pointer to an array
//! of type `bucket_type` to be passed in the constructor. This bucket array must
//! have at least the same lifetime as the container. This makes the use of
//! unordered_multiset more complicated than purely intrusive containers.
//! `bucket_type` is default-constructible, copyable and assignable
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>, \c hash<> and \c equal<>
//! \c bucket_traits<>, \c power_2_buckets<> and \c cache_begin<>.
//!
//! unordered_multiset only provides forward iterators but it provides 4 iterator types:
//! iterator and const_iterator to navigate through the whole container and
//! local_iterator and const_local_iterator to navigate through the values
//! stored in a single bucket. Local iterators are faster and smaller.
//!
//! It's not recommended to use non constant-time size unordered_multisets because several
//! key functions, like "empty()", become non-constant time functions. Non
//! constant-time size unordered_multisets are mainly provided to support auto-unlink hooks.
//!
//! unordered_multiset, unlike std::unordered_set, does not make automatic rehashings nor
//! offers functions related to a load factor. Rehashing can be explicitly requested
//! and the user must provide a new bucket array that will be used from that moment.
//!
//! Since no automatic rehashing is done, iterators are never invalidated when
//! inserting or erasing elements. Iterators are only invalidated when rehasing.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyHash, class VoidOrKeyEqual, class TSizeType, class TBucketTraits, std::size_t BoolFlags>
#endif
class UnorderedMultisetImpl
   #ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public HashtableImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags>
   #endif
{
   /// @cond
   private:
   typedef HashtableImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyHash, VoidOrKeyEqual, TBucketTraits, TSizeType, BoolFlags> TableType;
   /// @endcond

   //Movable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(UnorderedMultisetImpl)

   typedef TableType ImplementationDefined;

   public:
   typedef typename ImplementationDefined::ValueType                  ValueType;
   typedef typename ImplementationDefined::KeyType                    KeyType;
   typedef typename ImplementationDefined::ValueTraits                ValueTraits;
   typedef typename ImplementationDefined::BucketTraits               BucketTraits;
   typedef typename ImplementationDefined::Pointer                     Pointer;
   typedef typename ImplementationDefined::ConstPointer               ConstPointer;
   typedef typename ImplementationDefined::Reference                   Reference;
   typedef typename ImplementationDefined::ConstReference             ConstReference;
   typedef typename ImplementationDefined::DifferenceType             DifferenceType;
   typedef typename ImplementationDefined::SizeType                   SizeType;
   typedef typename ImplementationDefined::KeyEqual                   KeyEqual;
   typedef typename ImplementationDefined::Hasher                      Hasher;
   typedef typename ImplementationDefined::BucketType                 BucketType;
   typedef typename ImplementationDefined::BucketPtr                  BucketPtr;
   typedef typename ImplementationDefined::Iterator                    Iterator;
   typedef typename ImplementationDefined::ConstIterator              ConstIterator;
   typedef typename ImplementationDefined::InsertCommitData          InsertCommitData;
   typedef typename ImplementationDefined::LocalIterator              LocalIterator;
   typedef typename ImplementationDefined::ConstLocalIterator        ConstLocalIterator;
   typedef typename ImplementationDefined::NodeTraits                 NodeTraits;
   typedef typename ImplementationDefined::Node                        Node;
   typedef typename ImplementationDefined::NodePtr                    NodePtr;
   typedef typename ImplementationDefined::ConstNodePtr              ConstNodePtr;

   public:

   //! @copydoc ::boost::intrusive::hashtable::hashtable(const bucket_traits &,const hasher &,const key_equal &,const value_traits &)
   inline explicit UnorderedMultisetImpl ( const BucketTraits &b_traits
                                    , const Hasher & hash_func = Hasher()
                                    , const KeyEqual &equal_func = KeyEqual()
                                    , const ValueTraits &v_traits = ValueTraits())
      :  TableType(b_traits, hash_func, equal_func, v_traits)
   {}

   //! @copydoc ::boost::intrusive::hashtable::hashtable(bool,Iterator,Iterator,const bucket_traits &,const hasher &,const key_equal &,const value_traits &)
   template<class Iterator>
   inline UnorderedMultisetImpl ( Iterator b
                           , Iterator e
                           , const BucketTraits &b_traits
                           , const Hasher & hash_func = Hasher()
                           , const KeyEqual &equal_func = KeyEqual()
                           , const ValueTraits &v_traits = ValueTraits())
      :  TableType(false, b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!
   inline UnorderedMultisetImpl(BOOST_RV_REF(UnorderedMultisetImpl) x)
      :  TableType(BOOST_MOVE_BASE(TableType, x))
   {}

   //! <b>Effects</b>: to-do
   //!
   inline UnorderedMultisetImpl& operator=(BOOST_RV_REF(UnorderedMultisetImpl) x)
   {  return static_cast<UnorderedMultisetImpl&>(TableType::operator=(BOOST_MOVE_BASE(TableType, x)));  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::hashtable::~hashtable()
   ~UnorderedMultisetImpl();

   //! @copydoc ::boost::intrusive::hashtable::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::hash_function()const
   hasher hash_function() const;

   //! @copydoc ::boost::intrusive::hashtable::key_eq()const
   key_equal key_eq() const;

   //! @copydoc ::boost::intrusive::hashtable::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::hashtable
   void swap(UnorderedMultisetImpl& other);

   //! @copydoc ::boost::intrusive::hashtable::clone_from(const hashtable&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const UnorderedMultisetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TableType::clone_from;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::hashtable::clone_from(hashtable&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(UnorderedMultisetImpl) src, Cloner cloner, Disposer disposer)
   {  TableType::clone_from(BOOST_MOVE_BASE(TableType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::hashtable::insert_equal(reference)
   inline Iterator insert(Reference value)
   {  return TableType::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::hashtable::insert_equal(Iterator,Iterator)
   template<class Iterator>
   inline void insert(Iterator b, Iterator e)
   {  TableType::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::hashtable::erase(const_iterator)
   void erase(const_iterator i);

   //! @copydoc ::boost::intrusive::hashtable::erase(const_iterator,const_iterator)
   void erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::hashtable::erase(const KeyType&,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   size_type erase(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func);

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   BOOST_INTRUSIVE_DOC1ST(void
      , typename detail::disable_if_convertible<Disposer BOOST_INTRUSIVE_I const_iterator>::type)
      erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   void erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const key_type &,Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::hashtable::erase_and_dispose(const KeyType&,KeyHasher,KeyEqual,Disposer)
   template<class KeyType, class KeyHasher, class KeyEqual, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func, Disposer disposer);

   //! @copydoc ::boost::intrusive::hashtable::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::count(const key_type &)const
   size_type count(const key_type &key) const;

   //! @copydoc ::boost::intrusive::hashtable::count(const KeyType&,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   size_type count(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const;

   //! @copydoc ::boost::intrusive::hashtable::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::hashtable::find(const KeyType &,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   iterator find(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func);

   //! @copydoc ::boost::intrusive::hashtable::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::hashtable::find(const KeyType &,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   const_iterator find(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const;

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const key_type&)
   std::pair<iterator,iterator> equal_range(const key_type &key);

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const KeyType &,KeyHasher,KeyEqual)
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func);

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const key_type&)const
   std::pair<const_iterator, const_iterator>
      equal_range(const key_type &key) const;

   //! @copydoc ::boost::intrusive::hashtable::equal_range(const KeyType &,KeyHasher,KeyEqual)const
   template<class KeyType, class KeyHasher, class KeyEqual>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyHasher hash_func, KeyEqual equal_func) const;

   //! @copydoc ::boost::intrusive::hashtable::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::s_local_iterator_to(reference)
   static local_iterator s_local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::s_local_iterator_to(const_reference)
   static const_local_iterator s_local_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::local_iterator_to(reference)
   local_iterator local_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::local_iterator_to(const_reference)
   const_local_iterator local_iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket_count
   size_type bucket_count() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket_size
   size_type bucket_size(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::bucket(const key_type&)const
   size_type bucket(const key_type& k) const;

   //! @copydoc ::boost::intrusive::hashtable::bucket(const KeyType&,KeyHasher)const
   template<class KeyType, class KeyHasher>
   size_type bucket(const KeyType& k, KeyHasher hash_func) const;

   //! @copydoc ::boost::intrusive::hashtable::bucket_pointer
   bucket_ptr bucket_pointer() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin(size_type)
   local_iterator begin(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::begin(size_type)const
   const_local_iterator begin(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cbegin(size_type)const
   const_local_iterator cbegin(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end(size_type)
   local_iterator end(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::end(size_type)const
   const_local_iterator end(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::cend(size_type)const
   const_local_iterator cend(size_type n) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::rehash(const bucket_traits &)
   void rehash(const bucket_traits &new_bucket_traits);

   //! @copydoc ::boost::intrusive::hashtable::full_rehash
   void full_rehash();

   //! @copydoc ::boost::intrusive::hashtable::incremental_rehash(bool)
   bool incremental_rehash(bool grow = true);

   //! @copydoc ::boost::intrusive::hashtable::incremental_rehash(const bucket_traits &)
   bool incremental_rehash(const bucket_traits &new_bucket_traits);

   //! @copydoc ::boost::intrusive::hashtable::split_count
   size_type split_count() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::suggested_upper_bucket_count
   static size_type suggested_upper_bucket_count(size_type n) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::hashtable::suggested_lower_bucket_count
   static size_type suggested_lower_bucket_count(size_type n) BOOST_NOEXCEPT;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

//! Helper metafunction to define an \c unordered_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void, class O8 = void
                , class O9 = void, class O10= void
                , class O11 = void
                >
#endif
struct MakeUnorderedMultiset
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

   typedef UnorderedMultisetImpl
      < ValueTraits
      , typename PackedOptions::key_of_value
      , typename PackedOptions::hash
      , typename PackedOptions::equal
      , typename PackedOptions::size_type
      , BucketTraits
      ,  (std::size_t(false)*HashBoolFlags::UNIQUE_KEYS_POS)
      |  (std::size_t(PackedOptions::constant_time_size)*HashBoolFlags::CONSTANT_TIME_SIZE_POS)
      |  (std::size_t(PackedOptions::power_2_buckets)*HashBoolFlags::POWER_2_BUCKETS_POS)
      |  (std::size_t(PackedOptions::cache_begin)*HashBoolFlags::CACHE_BEGIN_POS)
      |  (std::size_t(PackedOptions::compare_hash)*HashBoolFlags::COMPARE_HASH_POS)
      |  (std::size_t(PackedOptions::incremental)*HashBoolFlags::INCREMENTAL_POS)
      |  (std::size_t(PackedOptions::linear_buckets)*HashBoolFlags::LINEAR_BUCKETS_POS)
      |  (std::size_t(PackedOptions::fastmod_buckets)*HashBoolFlags::FASTMOD_BUCKETS_POS)
      > ImplementationDefined;

   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7, class O8, class O9, class O10, class O11>
#else
template<class T, class ...Options>
#endif
class UnorderedMultiset
   :  public MakeUnorderedMultiset<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10, O11
         #else
         Options...
         #endif
      >::type
{
   typedef typename MakeUnorderedMultiset
      <T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6, O7, O8, O9, O10
         #else
         Options...
         #endif
      >::type   Base;
   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(UnorderedMultiset)

   public:
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::BucketTraits      BucketTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::BucketPtr         BucketPtr;
   typedef typename Base::SizeType          SizeType;
   typedef typename Base::Hasher             Hasher;
   typedef typename Base::KeyEqual          KeyEqual;

   inline
   explicit UnorderedMultiset( const BucketTraits &b_traits
                              , const Hasher & hash_func = Hasher()
                              , const KeyEqual &equal_func = KeyEqual()
                              , const ValueTraits &v_traits = ValueTraits())
      :  Base(b_traits, hash_func, equal_func, v_traits)
   {}

   template<class Iterator>
   inline
   UnorderedMultiset( Iterator b
                     , Iterator e
                     , const BucketTraits &b_traits
                     , const Hasher & hash_func = Hasher()
                     , const KeyEqual &equal_func = KeyEqual()
                     , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, b_traits, hash_func, equal_func, v_traits)
   {}

   inline UnorderedMultiset(BOOST_RV_REF(UnorderedMultiset) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline UnorderedMultiset& operator=(BOOST_RV_REF(UnorderedMultiset) x)
   {  return static_cast<UnorderedMultiset&>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const UnorderedMultiset &src, Cloner cloner, Disposer disposer)
   {  this->Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(UnorderedMultiset) src, Cloner cloner, Disposer disposer)
   {  this->Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_UNORDERED_SET_HPP
