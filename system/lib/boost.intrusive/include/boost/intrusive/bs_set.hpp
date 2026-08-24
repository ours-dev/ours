/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2013-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_BS_SET_HPP
#define BOOST_INTRUSIVE_BS_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/move/utility_core.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class ValueTraits, class VoidOrKeyOfValue, class Compare, class SizeType, bool ConstantTimeSize, typename HeaderHolder>
class bs_multiset_impl;
#endif

namespace boost {
namespace intrusive {

//! The class template bs_set is an intrusive container, that mimics most of
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
class BsSetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, BsTreeAlgorithmsValue, HeaderHolder>
#endif
{
   /// @cond
   typedef BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, BsTreeAlgorithmsValue, HeaderHolder> TreeType;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(BsSetImpl)

   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::KeyType                 KeyType;
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
   //! @copydoc ::boost::intrusive::bstree::bstree()
   BsSetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(const key_compare &,const value_traits &)
   explicit BsSetImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   BsSetImpl( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const ValueTraits &v_traits = ValueTraits())
      : TreeType(true, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   BsSetImpl(BOOST_RV_REF(BsSetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   BsSetImpl& operator=(BOOST_RV_REF(BsSetImpl) x)
   {  return static_cast<BsSetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~BsSetImpl();

   //! @copydoc ::boost::intrusive::bstree::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(iterator)
   static BsSetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const BsSetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static BsSetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const BsSetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::swap
   void swap(BsSetImpl& other);

   //! @copydoc ::boost::intrusive::bstree::clone_from(const bstree&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const BsSetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::clone_from(bstree&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(BsSetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(reference)
   std::pair<Iterator, bool> insert(Reference value)
   {  return TreeType::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_unique(hint, value);  }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_check
      (const KeyType &key, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, commit_data); }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_check
      (ConstIterator hint, const KeyType &key
      ,InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, commit_data); }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_check
      (const KeyType &key, KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, comp, commit_data); }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_check
      (ConstIterator hint, const KeyType &key
      ,KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, comp, commit_data); }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_commit
   Iterator insert_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return TreeType::insert_unique_commit(value, commit_data);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::count(const key_type &)const
   SizeType count(const KeyType &key) const
   {  return static_cast<SizeType>(this->TreeType::find(key) != this->TreeType::cend()); }

   //! @copydoc ::boost::intrusive::bstree::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   SizeType count(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return static_cast<SizeType>(this->TreeType::find(key, comp) != this->TreeType::cend()); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const key_type &)
   iterator lower_bound(const key_type &);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::equal_range(const key_type &)
   std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp)
   {  return this->TreeType::equal_range(key, comp); }

   //! @copydoc ::boost::intrusive::bstree::equal_range(const key_type &)const
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return this->TreeType::equal_range(key, comp); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type&,const key_type&,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type& lower_key, const key_type& upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type&,const key_type&,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type& lower_key, const key_type& upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::merge_unique
   template<class ...Options2>
   void merge(bs_set<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::bstree::merge_unique
   template<class ...Options2>
   void merge(bs_multiset<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(BsSetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }


   template<class Compare2>
   void merge(bs_multiset_impl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const bs_set_impl<T, Options...> &x, const bs_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const bs_set_impl<T, Options...> &x, const bs_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const bs_set_impl<T, Options...> &x, const bs_set_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const bs_set_impl<T, Options...> &x, const bs_set_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(bs_set_impl<T, Options...> &x, bs_set_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c bs_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeBsSet
{
   /// @cond
   typedef typename PackOptions
      < BstreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef BsSetImpl
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
class BsSet
   :  public MakeBsSet<T,
   #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
   O1, O2, O3, O4, O5, O6
   #else
   Options...
   #endif
   >::type
{
   typedef typename MakeBsSet
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(BsSet)
   public:
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline BsSet()
      :  Base()
   {}

   inline explicit BsSet( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline BsSet( Iterator b, Iterator e
      , const KeyCompare &cmp = KeyCompare()
      , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, v_traits)
   {}

   inline BsSet(BOOST_RV_REF(BsSet) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline BsSet& operator=(BOOST_RV_REF(BsSet) x)
   {  return static_cast<BsSet &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const BsSet &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(BsSet) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static BsSet &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<BsSet &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const BsSet &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const BsSet &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static BsSet &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<BsSet &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const BsSet &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const BsSet &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template bs_multiset is an intrusive container, that mimics most of
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
class BsMultisetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, BsTreeAlgorithmsValue, HeaderHolder>
#endif
{
   /// @cond
   typedef BstreeImpl<TValueTraits, VoidOrKeyOfValue, Compare, TSizeType, ConstantTimeSize, BsTreeAlgorithmsValue, HeaderHolder> TreeType;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(BsMultisetImpl)
   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::KeyType                 KeyType;
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
   //! @copydoc ::boost::intrusive::bstree::bstree()
   BsMultisetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(const key_compare &,const value_traits &)
   explicit BsMultisetImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   BsMultisetImpl( Iterator b, Iterator e
                , const KeyCompare &cmp = KeyCompare()
                , const ValueTraits &v_traits = ValueTraits())
      : TreeType(false, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   BsMultisetImpl(BOOST_RV_REF(BsMultisetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   BsMultisetImpl& operator=(BOOST_RV_REF(BsMultisetImpl) x)
   {  return static_cast<BsMultisetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~BsMultisetImpl();

   //! @copydoc ::boost::intrusive::bstree::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(iterator)
   static BsMultisetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const BsMultisetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static BsMultisetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const BsMultisetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::swap
   void swap(BsMultisetImpl& other);

   //! @copydoc ::boost::intrusive::bstree::clone_from(const bstree&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const BsMultisetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::clone_from(bstree&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(BsMultisetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(reference)
   Iterator insert(Reference value)
   {  return TreeType::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_equal(hint, value);  }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::bstree::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::count(const key_type &)const
   size_type count(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   size_type count(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const key_type &)
   iterator lower_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const key_type &)
   std::pair<iterator,iterator> equal_range(const key_type &key);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::bstree::equal_range(const key_type &)const
   std::pair<const_iterator, const_iterator>
      equal_range(const key_type &key) const;

   //! @copydoc ::boost::intrusive::bstree::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type & lower_key, const key_type & upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type & lower_key, const key_type & upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::merge_equal
   template<class ...Options2>
   void merge(bs_multiset<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::bstree::merge_equal
   template<class ...Options2>
   void merge(bs_set<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(BsMultisetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   template<class Compare2>
   void merge(BsSetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};

#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

template<class T, class ...Options>
bool operator!= (const bs_multiset_impl<T, Options...> &x, const bs_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>(const bs_multiset_impl<T, Options...> &x, const bs_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator<=(const bs_multiset_impl<T, Options...> &x, const bs_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
bool operator>=(const bs_multiset_impl<T, Options...> &x, const bs_multiset_impl<T, Options...> &y);

template<class T, class ...Options>
void swap(bs_multiset_impl<T, Options...> &x, bs_multiset_impl<T, Options...> &y);

#endif   //#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

//! Helper metafunction to define a \c bs_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeBsMultiset
{
   /// @cond
   typedef typename PackOptions
      < BstreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef BsMultisetImpl
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
class BsMultiset
   :  public MakeBsMultiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeBsMultiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(BsMultiset)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline BsMultiset()
      :  Base()
   {}

   inline explicit BsMultiset( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline BsMultiset( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, v_traits)
   {}

   inline BsMultiset(BOOST_RV_REF(BsMultiset) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline BsMultiset& operator=(BOOST_RV_REF(BsMultiset) x)
   {  return static_cast<BsMultiset &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const BsMultiset &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(BsMultiset) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static BsMultiset &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<BsMultiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const BsMultiset &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const BsMultiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static BsMultiset &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<BsMultiset &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const BsMultiset &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const BsMultiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_BS_SET_HPP
