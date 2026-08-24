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
#ifndef BOOST_INTRUSIVE_SET_HPP
#define BOOST_INTRUSIVE_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/rbtree.hpp>
#include <boost/move/utility_core.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class ValueTraits, class VoidOrKeyOfValue, class Compare, class SizeType, bool ConstantTimeSize, typename HeaderHolder>
class MultisetImpl;
#endif

//! The class template set is an intrusive container, that mimics most of
//! the interface of std::set as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class Compare, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class SetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, RbTreeAlgorithmsValue, HeaderHolder>
#endif
{
   /// @cond
   typedef BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, RbTreeAlgorithmsValue, HeaderHolder> TreeType;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(SetImpl)

   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::KeyType                 KeyType;
   typedef typename ImplementationDefined::KeyOfValue             KeyOfValue;
   typedef typename ImplementationDefined::ValueTraits             ValueTraits;
   typedef typename ImplementationDefined::Pointer                  Pointer;
   typedef typename ImplementationDefined::ConstPointer            ConstPointer;
   typedef typename ImplementationDefined::Reference                Reference;
   typedef typename ImplementationDefined::ConstReference          ConstReference;
   typedef typename ImplementationDefined::DifferenceType          DifferenceType;
   typedef typename ImplementationDefined::SizeType                SizeType;
   typedef typename ImplementationDefined::ValueCompare            ValueCompare;
   typedef typename ImplementationDefined::KeyCompare              KeyCompare;
   typedef typename ImplementationDefined::Iterator                 Iterator;
   typedef typename ImplementationDefined::ConstIterator           ConstIterator;
   typedef typename ImplementationDefined::ReverseIterator         ReverseIterator;
   typedef typename ImplementationDefined::ConstReverseIterator   ConstReverseIterator;
   typedef typename ImplementationDefined::InsertCommitData       InsertCommitData;
   typedef typename ImplementationDefined::NodeTraits              NodeTraits;
   typedef typename ImplementationDefined::Node                     Node;
   typedef typename ImplementationDefined::NodePtr                 NodePtr;
   typedef typename ImplementationDefined::ConstNodePtr           ConstNodePtr;
   typedef typename ImplementationDefined::NodeAlgorithms          NodeAlgorithms;

   static const bool CONSTANT_TIME_SIZE = TreeType::CONSTANT_TIME_SIZE;

   public:
   //! @copydoc ::boost::intrusive::rbtree::rbtree()
   SetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(const key_compare &,const value_traits &)
   explicit SetImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   SetImpl( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const ValueTraits &v_traits = ValueTraits())
      : TreeType(true, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(rbtree &&)
   SetImpl(BOOST_RV_REF(SetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! @copydoc ::boost::intrusive::rbtree::operator=(rbtree &&)
   SetImpl& operator=(BOOST_RV_REF(SetImpl) x)
   {  return static_cast<SetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::rbtree::~rbtree()
   ~SetImpl();

   //! @copydoc ::boost::intrusive::rbtree::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_end_iterator(iterator)
   static SetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_end_iterator(const_iterator)
   static const SetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_iterator(iterator)
   static SetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_iterator(const_iterator)
   static const SetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::rbtree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::rbtree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::swap
   void swap(SetImpl& other);

   //! @copydoc ::boost::intrusive::rbtree::clone_from(const rbtree&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const SetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::clone_from(rbtree&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(SetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique(reference)
   std::pair<Iterator, bool> insert(Reference value)
   {  return TreeType::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_unique(hint, value);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique_check(const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_check
      (const KeyType &key, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, commit_data); }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique_check(const_iterator,const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_check
      (ConstIterator hint, const KeyType &key
      ,InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, commit_data); }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique_check(const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_check
      (const KeyType &key, KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, comp, commit_data); }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique_check(const_iterator,const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_check
      (ConstIterator hint, const KeyType &key
      ,KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, comp, commit_data); }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_unique_commit
   Iterator insert_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return TreeType::insert_unique_commit(value, commit_data);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::rbtree::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::rbtree::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::count(const key_type &)const
   SizeType count(const KeyType &key) const
   {  return static_cast<SizeType>(this->TreeType::find(key) != this->TreeType::cend()); }

   //! @copydoc ::boost::intrusive::rbtree::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   SizeType count(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return static_cast<SizeType>(this->TreeType::find(key, comp) != this->TreeType::cend()); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const key_type &)
   iterator lower_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const key_type &)
   std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp)
   {  return this->TreeType::equal_range(key, comp); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const key_type &)const
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return this->TreeType::equal_range(key, comp); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::rbtree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::merge_unique
   template<class ...Options2>
   void merge(set<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::rbtree::merge_unique
   template<class ...Options2>
   void merge(multiset<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(SetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }


   template<class Compare2>
   void merge(MultisetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const set_impl<T, Options...> &x, const set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const set_impl<T, Options...> &x, const set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const set_impl<T, Options...> &x, const set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const set_impl<T, Options...> &x, const set_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(set_impl<T, Options...> &x, set_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeSet
{
   /// @cond
   typedef typename PackOptions
      < RbtreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef SetImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::size_type
         , PackedOptions::constant_time_size
         , typename PackedOptions::header_holder_type
         > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6>
#else
template<class T, class ...Options>
#endif
class Set
   :  public MakeSet<T,
   #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
   O1, O2, O3, O4, O5, O6
   #else
   Options...
   #endif
   >::type
{
   typedef typename MakeSet
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(Set)
   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Set()
      :  Base()
   {}

   inline explicit Set( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline Set( Iterator b, Iterator e
      , const KeyCompare &cmp = KeyCompare()
      , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, v_traits)
   {}

   inline Set(BOOST_RV_REF(Set) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Set& operator=(BOOST_RV_REF(Set) x)
   {  return static_cast<Set &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Set &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Set) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Set &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Set &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Set &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Set &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Set &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Set &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Set &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Set &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template multiset is an intrusive container, that mimics most of
//! the interface of std::multiset as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class Compare, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class MultisetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, RbTreeAlgorithmsValue, HeaderHolder>
#endif
{
   /// @cond
   typedef BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, RbTreeAlgorithmsValue, HeaderHolder> TreeType;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(MultisetImpl)
   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::KeyType                 KeyType;
   typedef typename ImplementationDefined::KeyOfValue             KeyOfValue;
   typedef typename ImplementationDefined::ValueTraits             ValueTraits;
   typedef typename ImplementationDefined::Pointer                  Pointer;
   typedef typename ImplementationDefined::ConstPointer            ConstPointer;
   typedef typename ImplementationDefined::Reference                Reference;
   typedef typename ImplementationDefined::ConstReference          ConstReference;
   typedef typename ImplementationDefined::DifferenceType          DifferenceType;
   typedef typename ImplementationDefined::SizeType                SizeType;
   typedef typename ImplementationDefined::ValueCompare            ValueCompare;
   typedef typename ImplementationDefined::KeyCompare              KeyCompare;
   typedef typename ImplementationDefined::Iterator                 Iterator;
   typedef typename ImplementationDefined::ConstIterator           ConstIterator;
   typedef typename ImplementationDefined::ReverseIterator         ReverseIterator;
   typedef typename ImplementationDefined::ConstReverseIterator   ConstReverseIterator;
   typedef typename ImplementationDefined::InsertCommitData       InsertCommitData;
   typedef typename ImplementationDefined::NodeTraits              NodeTraits;
   typedef typename ImplementationDefined::Node                     Node;
   typedef typename ImplementationDefined::NodePtr                 NodePtr;
   typedef typename ImplementationDefined::ConstNodePtr           ConstNodePtr;
   typedef typename ImplementationDefined::NodeAlgorithms          NodeAlgorithms;

   static const bool CONSTANT_TIME_SIZE = TreeType::CONSTANT_TIME_SIZE;

   public:
   //! @copydoc ::boost::intrusive::rbtree::rbtree()
   MultisetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(const key_compare &,const value_traits &)
   explicit MultisetImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   MultisetImpl( Iterator b, Iterator e
                , const KeyCompare &cmp = KeyCompare()
                , const ValueTraits &v_traits = ValueTraits())
      : TreeType(false, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::rbtree::rbtree(rbtree &&)
   MultisetImpl(BOOST_RV_REF(MultisetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! @copydoc ::boost::intrusive::rbtree::operator=(rbtree &&)
   MultisetImpl& operator=(BOOST_RV_REF(MultisetImpl) x)
   {  return static_cast<MultisetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::rbtree::~rbtree()
   ~MultisetImpl();

   //! @copydoc ::boost::intrusive::rbtree::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_end_iterator(iterator)
   static MultisetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_end_iterator(const_iterator)
   static const MultisetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_iterator(iterator)
   static MultisetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::container_from_iterator(const_iterator)
   static const MultisetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::rbtree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::rbtree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::swap
   void swap(MultisetImpl& other);

   //! @copydoc ::boost::intrusive::rbtree::clone_from(const rbtree&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const MultisetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::rbtree::clone_from(rbtree&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(MultisetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_equal(reference)
   Iterator insert(Reference value)
   {  return TreeType::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_equal(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_equal(hint, value);  }

   //! @copydoc ::boost::intrusive::rbtree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::rbtree::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::rbtree::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::rbtree::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::count(const key_type &)const
   size_type count(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   size_type count(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const key_type &)
   iterator lower_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const key_type &)
   std::pair<iterator,iterator> equal_range(const key_type &key);

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const key_type &)const
   std::pair<const_iterator, const_iterator>
      equal_range(const key_type &key) const;

   //! @copydoc ::boost::intrusive::rbtree::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::rbtree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::rbtree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::rbtree::merge_equal
   template<class ...Options2>
   void merge(multiset<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::rbtree::merge_equal
   template<class ...Options2>
   void merge(set<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(MultisetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   template<class Compare2>
   void merge(SetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const multiset_impl<T, Options...> &x, const multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const multiset_impl<T, Options...> &x, const multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const multiset_impl<T, Options...> &x, const multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const multiset_impl<T, Options...> &x, const multiset_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(multiset_impl<T, Options...> &x, multiset_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeMultiset
{
   /// @cond
   typedef typename PackOptions
      < RbtreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef MultisetImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::size_type
         , PackedOptions::constant_time_size
         , typename PackedOptions::header_holder_type
         > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6>
#else
template<class T, class ...Options>
#endif
class Multiset
   :  public MakeMultiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeMultiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(Multiset)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Multiset()
      :  Base()
   {}

   inline explicit Multiset( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline Multiset( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, v_traits)
   {}

   inline Multiset(BOOST_RV_REF(Multiset) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Multiset& operator=(BOOST_RV_REF(Multiset) x)
   {  return static_cast<Multiset &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Multiset &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Multiset) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Multiset &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Multiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Multiset &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Multiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Multiset &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Multiset &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Multiset &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Multiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SET_HPP
