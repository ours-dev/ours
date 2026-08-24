/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_TREAP_SET_HPP
#define BOOST_INTRUSIVE_TREAP_SET_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/treap.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/move/utility_core.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

#if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class ValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class VoidOrPrioOfValue, class VoidOrPrioComp, class SizeType, bool ConstantTimeSize, typename HeaderHolder>
class TreapMultisetImpl;
#endif

//! The class template treap_set is an intrusive container, that mimics most of
//! the interface of std::set as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>,
//! \c compare<>, \c priority<> and \c priority_of_value<>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class VoidOrPrioOfValue, class VoidOrPrioComp, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class TreapSetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public TreapImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder>
#endif
{
   /// @cond
   public:
   typedef TreapImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> TreeType;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(TreapSetImpl)

   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::ValueTraits             ValueTraits;
   typedef typename ImplementationDefined::KeyType                 KeyType;
   typedef typename ImplementationDefined::KeyOfValue             KeyOfValue;
   typedef typename ImplementationDefined::Pointer                  Pointer;
   typedef typename ImplementationDefined::ConstPointer            ConstPointer;
   typedef typename ImplementationDefined::Reference                Reference;
   typedef typename ImplementationDefined::ConstReference          ConstReference;
   typedef typename ImplementationDefined::DifferenceType          DifferenceType;
   typedef typename ImplementationDefined::SizeType                SizeType;
   typedef typename ImplementationDefined::ValueCompare            ValueCompare;
   typedef typename ImplementationDefined::KeyCompare              KeyCompare;
   typedef typename ImplementationDefined::PriorityType            PriorityType;
   typedef typename ImplementationDefined::PriorityCompare         PriorityCompare;
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

   static const bool CONSTANT_TIME_SIZE = ImplementationDefined::CONSTANT_TIME_SIZE;

   public:
   //! @copydoc ::boost::intrusive::treap::treap()
   TreapSetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::treap::treap(const key_compare &,const priority_compare &,const value_traits &)
   explicit TreapSetImpl( const KeyCompare &cmp
                          , const PriorityCompare &pcmp  = PriorityCompare()
                          , const ValueTraits &v_traits  = ValueTraits())
      :  TreeType(cmp, pcmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::treap::treap(bool,Iterator,Iterator,const key_compare &,const priority_compare &,const value_traits &)
   template<class Iterator>
   TreapSetImpl( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const PriorityCompare &pcmp  = PriorityCompare()
           , const ValueTraits &v_traits = ValueTraits())
      : TreeType(true, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!
   TreapSetImpl(BOOST_RV_REF(TreapSetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! <b>Effects</b>: to-do
   //!
   TreapSetImpl& operator=(BOOST_RV_REF(TreapSetImpl) x)
   {  return static_cast<TreapSetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x)));  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::~treap()
   ~TreapSetImpl();

   //! @copydoc ::boost::intrusive::treap::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(iterator)
   static TreapSetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(const_iterator)
   static const TreapSetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(iterator)
   static TreapSetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(const_iterator)
   static const TreapSetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::treap::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::treap::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::swap
   void swap(TreapSetImpl& other);

   //! @copydoc ::boost::intrusive::treap::clone_from(const treap&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const TreapSetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif

   //! @copydoc ::boost::intrusive::treap::clone_from(treap&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(TreapSetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! @copydoc ::boost::intrusive::treap::top()
   inline iterator top() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::top()const
   inline const_iterator top() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::ctop()const
   inline const_iterator ctop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rtop()
   inline reverse_iterator rtop() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rtop()const
   inline const_reverse_iterator rtop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crtop()const
   inline const_reverse_iterator crtop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crtop() const
   priority_compare priority_comp() const;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::insert_unique(reference)
   std::pair<Iterator, bool> insert(Reference value)
   {  return TreeType::insert_unique(value);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_unique(hint, value);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const key_type&,const priority_type &,insert_commit_data&)
   std::pair<Iterator, bool> insert_check( const KeyType &key, const PriorityType &prio, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, prio, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const_iterator,const key_type&,const priority_type &,insert_commit_data&)
   std::pair<Iterator, bool> insert_check
      ( ConstIterator hint, const KeyType &key, const PriorityType &prio, InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, prio, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const KeyType&,KeyTypeKeyCompare,PrioValuePrioCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare, class PrioType, class PrioValuePrioCompare>
   std::pair<Iterator, bool> insert_check
      ( const KeyType &key, KeyTypeKeyCompare comp, const PrioType &prio, PrioValuePrioCompare pcomp
      , InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(key, comp, prio, pcomp, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique_check(const_iterator,const KeyType&,KeyTypeKeyCompare,PrioValuePrioCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare, class PrioType, class PrioValuePrioCompare>
   std::pair<Iterator, bool> insert_check
      ( ConstIterator hint
      , const KeyType &key, KeyTypeKeyCompare comp
      , const PrioType &prio, PrioValuePrioCompare pcomp
      , InsertCommitData &commit_data)
   {  return TreeType::insert_unique_check(hint, key, comp, prio, pcomp, commit_data); }

   //! @copydoc ::boost::intrusive::treap::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_unique(b, e);  }

   //! @copydoc ::boost::intrusive::treap::insert_unique_commit
   Iterator insert_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {  return TreeType::insert_unique_commit(value, commit_data);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::count(const key_type &)const
   SizeType count(const KeyType &key) const
   {  return static_cast<SizeType>(this->TreeType::find(key) != this->TreeType::cend()); }

   //! @copydoc ::boost::intrusive::treap::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   SizeType count(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return static_cast<SizeType>(this->TreeType::find(key, comp) != this->TreeType::cend()); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::lower_bound(const key_type &)
   iterator lower_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   #endif   //   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::equal_range(const key_type &)
   std::pair<Iterator,Iterator> equal_range(const KeyType &key)
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator,Iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp)
   {  return this->TreeType::equal_range(key, comp); }

   //! @copydoc ::boost::intrusive::treap::equal_range(const key_type &)const
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType &key) const
   {  return this->TreeType::lower_bound_range(key); }

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<ConstIterator, ConstIterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const
   {  return this->TreeType::equal_range(key, comp); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;


   //! @copydoc ::boost::intrusive::treap::merge_unique
   template<class ...Options2>
   void merge(treap_set<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::treap::merge_unique
   template<class ...Options2>
   void merge(treap_multiset<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(TreapSetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }

   template<class Compare2>
   void merge(TreapMultisetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_unique(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};


//! Helper metafunction to define a \c treap_set that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void>
#endif
struct MakeTreapSet
{
   typedef typename PackOptions
      < TreapDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef TreapSetImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::priority_of_value
         , typename PackedOptions::priority
         , typename PackedOptions::size_type
         , PackedOptions::constant_time_size
         , typename PackedOptions::header_holder_type
         > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7>
#else
template<class T, class ...Options>
#endif
class TreapSet
   :  public MakeTreapSet<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeTreapSet
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(TreapSet)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::PriorityCompare   PriorityCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline TreapSet()
      :  Base()
   {}

   inline explicit TreapSet( const KeyCompare &cmp
                     , const PriorityCompare &pcmp = PriorityCompare()
                     , const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   inline TreapSet( Iterator b, Iterator e
      , const KeyCompare &cmp = KeyCompare()
      , const PriorityCompare &pcmp = PriorityCompare()
      , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, pcmp, v_traits)
   {}

   inline TreapSet(BOOST_RV_REF(TreapSet) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline TreapSet& operator=(BOOST_RV_REF(TreapSet) x)
   {  return static_cast<TreapSet &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const TreapSet &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(TreapSet) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static TreapSet &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<TreapSet &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const TreapSet &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const TreapSet &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static TreapSet &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<TreapSet &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const TreapSet &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const TreapSet &>(Base::container_from_iterator(it));   }
};

#endif

//! The class template treap_multiset is an intrusive container, that mimics most of
//! the interface of std::treap_multiset as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>,
//! \c compare<>, \c priority<> and \c priority_of_value<>
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class VoidOrPrioOfValue, class VoidOrPrioComp, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class TreapMultisetImpl
#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   : public TreapImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder>
#endif
{
   /// @cond
   typedef TreapImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> TreeType;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(TreapMultisetImpl)

   typedef TreeType ImplementationDefined;
   /// @endcond

   public:
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::ValueTraits             ValueTraits;
   typedef typename ImplementationDefined::KeyType                 KeyType;
   typedef typename ImplementationDefined::KeyOfValue             KeyOfValue;
   typedef typename ImplementationDefined::Pointer                  Pointer;
   typedef typename ImplementationDefined::ConstPointer            ConstPointer;
   typedef typename ImplementationDefined::Reference                Reference;
   typedef typename ImplementationDefined::ConstReference          ConstReference;
   typedef typename ImplementationDefined::DifferenceType          DifferenceType;
   typedef typename ImplementationDefined::SizeType                SizeType;
   typedef typename ImplementationDefined::ValueCompare            ValueCompare;
   typedef typename ImplementationDefined::KeyCompare              KeyCompare;
   typedef typename ImplementationDefined::PriorityType            PriorityType;
   typedef typename ImplementationDefined::PriorityCompare         PriorityCompare;
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

   static const bool CONSTANT_TIME_SIZE = ImplementationDefined::CONSTANT_TIME_SIZE;

   public:

   //! @copydoc ::boost::intrusive::treap::treap()
   TreapMultisetImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::treap::treap(const key_compare &,const priority_compare &,const value_traits &)
   explicit TreapMultisetImpl( const KeyCompare &cmp
                          , const PriorityCompare &pcmp  = PriorityCompare()
                          , const ValueTraits &v_traits  = ValueTraits())
      :  TreeType(cmp, pcmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::treap::treap(bool,Iterator,Iterator,const key_compare &,const priority_compare &,const value_traits &)
   template<class Iterator>
   TreapMultisetImpl( Iterator b, Iterator e
           , const KeyCompare &cmp = KeyCompare()
           , const PriorityCompare &pcmp  = PriorityCompare()
           , const ValueTraits &v_traits = ValueTraits())
      : TreeType(false, b, e, cmp, pcmp, v_traits)
   {}

   //! <b>Effects</b>: to-do
   //!
   TreapMultisetImpl(BOOST_RV_REF(TreapMultisetImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! <b>Effects</b>: to-do
   //!
   TreapMultisetImpl& operator=(BOOST_RV_REF(TreapMultisetImpl) x)
   {  return static_cast<TreapMultisetImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x)));  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::~treap()
   ~TreapMultisetImpl();

   //! @copydoc ::boost::intrusive::treap::begin()
   iterator begin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::begin()const
   const_iterator begin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::cbegin()const
   const_iterator cbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::end()
   iterator end() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::end()const
   const_iterator end() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::cend()const
   const_iterator cend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rbegin()
   reverse_iterator rbegin() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rbegin()const
   const_reverse_iterator rbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crbegin()const
   const_reverse_iterator crbegin() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rend()
   reverse_iterator rend() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rend()const
   const_reverse_iterator rend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crend()const
   const_reverse_iterator crend() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::root()
   iterator root() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::root()const
   const_iterator root() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::croot()const
   const_iterator croot() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(iterator)
   static TreapMultisetImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_end_iterator(const_iterator)
   static const TreapMultisetImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(iterator)
   static TreapMultisetImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::container_from_iterator(const_iterator)
   static const TreapMultisetImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::treap::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::treap::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::swap
   void swap(TreapMultisetImpl& other);

   //! @copydoc ::boost::intrusive::treap::clone_from(const treap&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const TreapMultisetImpl &src, Cloner cloner, Disposer disposer);

   #else

   using TreeType::clone_from;

   #endif

   //! @copydoc ::boost::intrusive::treap::clone_from(treap&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(TreapMultisetImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)

   //! @copydoc ::boost::intrusive::treap::top()
   inline iterator top() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::top()const
   inline const_iterator top() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::ctop()const
   inline const_iterator ctop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rtop()
   inline reverse_iterator rtop() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::rtop()const
   inline const_reverse_iterator rtop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crtop()const
   inline const_reverse_iterator crtop() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::crtop() const
   priority_compare priority_comp() const;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::treap::insert_equal(reference)
   Iterator insert(Reference value)
   {  return TreeType::insert_equal(value);  }

   //! @copydoc ::boost::intrusive::treap::insert_equal(const_iterator,reference)
   Iterator insert(ConstIterator hint, Reference value)
   {  return TreeType::insert_equal(hint, value);  }

   //! @copydoc ::boost::intrusive::treap::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert(Iterator b, Iterator e)
   {  TreeType::insert_equal(b, e);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::treap::insert_before
   iterator insert_before(const_iterator pos, reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::push_back
   void push_back(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::push_front
   void push_front(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator)
   iterator erase(const_iterator i) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const_iterator,const_iterator)
   iterator erase(const_iterator b, const_iterator e) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase(const key_type &)
   size_type erase(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::erase(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   size_type erase(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator i, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   iterator erase_and_dispose(const_iterator b, const_iterator e, Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   size_type erase_and_dispose(const key_type &key, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   size_type erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer);

   //! @copydoc ::boost::intrusive::treap::clear
   void clear() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::count(const key_type &)const
   size_type count(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::count(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   size_type count(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::lower_bound(const key_type &)
   iterator lower_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::lower_bound(const key_type &)const
   const_iterator lower_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::lower_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator lower_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const key_type &)
   iterator upper_bound(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::upper_bound(const key_type &)const
   const_iterator upper_bound(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::upper_bound(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator upper_bound(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::find(const key_type &)
   iterator find(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   iterator find(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::find(const key_type &)const
   const_iterator find(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::find(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   const_iterator find(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const key_type &)
   std::pair<iterator,iterator> equal_range(const key_type &key);

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyTypeKeyCompare)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> equal_range(const KeyType& key, KeyTypeKeyCompare comp);

   //! @copydoc ::boost::intrusive::treap::equal_range(const key_type &)const
   std::pair<const_iterator, const_iterator>
      equal_range(const key_type &key) const;

   //! @copydoc ::boost::intrusive::treap::equal_range(const KeyType&,KeyTypeKeyCompare)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator>
      equal_range(const KeyType& key, KeyTypeKeyCompare comp) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const key_type &,const key_type &,bool,bool)
   std::pair<iterator,iterator> bounded_range
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::treap::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)const
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<const_iterator, const_iterator> bounded_range
         (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed) const;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(reference)
   static iterator s_iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::s_iterator_to(const_reference)
   static const_iterator s_iterator_to(const_reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::iterator_to(reference)
   iterator iterator_to(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::iterator_to(const_reference)const
   const_iterator iterator_to(const_reference value) const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::init_node(reference)
   static void init_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::unlink_leftmost_without_rebalance
   pointer unlink_leftmost_without_rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::replace_node
   void replace_node(iterator replace_this, reference with_this) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::remove_node
   void remove_node(reference value) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::treap::merge_unique
   template<class ...Options2>
   void merge(treap_multiset<T, Options2...> &source);

   //! @copydoc ::boost::intrusive::treap::merge_unique
   template<class ...Options2>
   void merge(treap_set<T, Options2...> &source);

   #else

   template<class Compare2>
   void merge(TreapMultisetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   template<class Compare2>
   void merge(TreapSetImpl<TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   {  return TreeType::merge_equal(source);  }

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};


//! Helper metafunction to define a \c treap_multiset that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void>
#endif
struct MakeTreapMultiset
{
   typedef typename PackOptions
      < TreapDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef TreapMultisetImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::priority_of_value
         , typename PackedOptions::priority
         , typename PackedOptions::size_type
         , PackedOptions::constant_time_size
         , typename PackedOptions::header_holder_type
         > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};

#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4, class O5, class O6, class O7>
#else
template<class T, class ...Options>
#endif
class TreapMultiset
   :  public MakeTreapMultiset<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeTreapMultiset
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(TreapMultiset)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::PriorityCompare   PriorityCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline TreapMultiset()
      :  Base()
   {}

   inline explicit TreapMultiset( const KeyCompare &cmp
                          , const PriorityCompare &pcmp = PriorityCompare()
                          , const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   inline TreapMultiset( Iterator b, Iterator e
      , const KeyCompare &cmp = KeyCompare()
      , const PriorityCompare &pcmp = PriorityCompare()
      , const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, cmp, pcmp, v_traits)
   {}

   inline TreapMultiset(BOOST_RV_REF(TreapMultiset) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline TreapMultiset& operator=(BOOST_RV_REF(TreapMultiset) x)
   {  return static_cast<TreapMultiset &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const TreapMultiset &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(TreapMultiset) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static TreapMultiset &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<TreapMultiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const TreapMultiset &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const TreapMultiset &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static TreapMultiset &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<TreapMultiset &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const TreapMultiset &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const TreapMultiset &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREAP_SET_HPP
