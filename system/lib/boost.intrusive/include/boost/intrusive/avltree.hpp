/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2007-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_AVLTREE_HPP
#define BOOST_INTRUSIVE_AVLTREE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <cstddef>
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>
#include <boost/intrusive/detail/minimal_pair_header.hpp>

#include <boost/intrusive/avl_set_hook.hpp>
#include <boost/intrusive/detail/avltree_node.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/avltree_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/move/utility_core.hpp>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

struct DefaultAvltreeHookApplier
{  template <class T> struct Apply{ typedef typename T::default_avltree_hook Type;  };  };

template<>
struct IsDefaultHookTag<DefaultAvltreeHookApplier>
{  static const bool VALUE = true;  };

struct AvltreeDefaults
   : BstreeDefaults
{
   typedef DefaultAvltreeHookApplier ProtoValueTraits;
};

/// @endcond

//! The class template avltree is an intrusive AVL tree container, that
//! is used to construct intrusive avl_set and avl_multiset containers.
//! The no-throw guarantee holds only, if the key_compare object
//! doesn't throw.
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
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class AvltreeImpl
   /// @cond
   :  public BstreeImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType, ConstantTimeSize, AvlTreeAlgorithmsValue, HeaderHolder>
   /// @endcond
{
   public:
   typedef TValueTraits ValueTraits;
   /// @cond
   typedef BstreeImpl< TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType
                      , ConstantTimeSize, AvlTreeAlgorithmsValue
                      , HeaderHolder>                                TreeType;
   typedef TreeType                                                 ImplementationDefined;
   /// @endcond

   typedef typename ImplementationDefined::Pointer                  Pointer;
   typedef typename ImplementationDefined::ConstPointer            ConstPointer;
   typedef typename ImplementationDefined::ValueType               ValueType;
   typedef typename ImplementationDefined::KeyType                 KeyType;
   typedef typename ImplementationDefined::KeyOfValue             KeyOfValue;
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
   typedef typename ImplementationDefined::NodeTraits              NodeTraits;
   typedef typename ImplementationDefined::Node                     Node;
   typedef typename ImplementationDefined::NodePtr                 NodePtr;
   typedef typename ImplementationDefined::ConstNodePtr           ConstNodePtr;
   typedef typename ImplementationDefined::NodeAlgorithms          NodeAlgorithms;

   static const bool CONSTANT_TIME_SIZE = ImplementationDefined::CONSTANT_TIME_SIZE;
   /// @cond
   private:

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(AvltreeImpl)

   /// @endcond

   public:

   typedef typename ImplementationDefined::InsertCommitData InsertCommitData;

   //! @copydoc ::boost::intrusive::bstree::bstree()
   AvltreeImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(const key_compare &,const value_traits &)
   explicit AvltreeImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   AvltreeImpl( bool unique, Iterator b, Iterator e
              , const KeyCompare &cmp     = KeyCompare()
              , const ValueTraits &v_traits = ValueTraits())
      : TreeType(unique, b, e, cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   AvltreeImpl(BOOST_RV_REF(AvltreeImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x))
   {}

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   AvltreeImpl& operator=(BOOST_RV_REF(AvltreeImpl) x)
   {  return static_cast<AvltreeImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x))); }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~AvltreeImpl();

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
   static AvltreeImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const AvltreeImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static AvltreeImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const AvltreeImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::swap
   void swap(AvltreeImpl& other);

   //! @copydoc ::boost::intrusive::bstree::clone_from(const bstree&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(const AvltreeImpl &src, Cloner cloner, Disposer disposer);

   #else //BOOST_INTRUSIVE_DOXYGEN_INVOKED

   using TreeType::clone_from;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::clone_from(bstree&&,Cloner,Disposer)
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(AvltreeImpl) src, Cloner cloner, Disposer disposer)
   {  TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::insert_equal(reference)
   iterator insert_equal(reference value);

   //! @copydoc ::boost::intrusive::bstree::insert_equal(const_iterator,reference)
   iterator insert_equal(const_iterator hint, reference value);

   //! @copydoc ::boost::intrusive::bstree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e);

   //! @copydoc ::boost::intrusive::bstree::insert_unique(reference)
   std::pair<iterator, bool> insert_unique(reference value);

   //! @copydoc ::boost::intrusive::bstree::insert_unique(const_iterator,reference)
   iterator insert_unique(const_iterator hint, reference value);

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator, bool> insert_unique_check
      (const KeyType &key, KeyTypeKeyCompare comp, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const KeyType &key
      ,KeyTypeKeyCompare comp, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const key_type&,insert_commit_data&)
   std::pair<iterator, bool> insert_unique_check
      (const key_type &key, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const key_type&,insert_commit_data&)
   std::pair<iterator, bool> insert_unique_check
      (const_iterator hint, const key_type &key, insert_commit_data &commit_data);

   //! @copydoc ::boost::intrusive::bstree::insert_unique_commit
   iterator insert_unique_commit(reference value, const insert_commit_data &commit_data) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::insert_unique(Iterator,Iterator)
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e);

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

   //! @copydoc ::boost::intrusive::bstree::count(const key_type &ke)const
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

   //! @copydoc ::boost::intrusive::bstree::upper_bound(const key_type &key)
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
      (const key_type &lower, const key_type &upper_key, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const KeyType&,const KeyType&,KeyTypeKeyCompare,bool,bool)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<iterator,iterator> bounded_range
      (const KeyType& lower_key, const KeyType& upper_key, KeyTypeKeyCompare comp, bool left_closed, bool right_closed);

   //! @copydoc ::boost::intrusive::bstree::bounded_range(const key_type &,const key_type &,bool,bool)const
   std::pair<const_iterator, const_iterator>
      bounded_range(const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed) const;

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

   //! @copydoc ::boost::intrusive::bstree::merge_unique(bstree<T, Options2...>&)
   template<class T, class ...Options2>
   void merge_unique(avltree<T, Options2...> &);

   //! @copydoc ::boost::intrusive::bstree::merge_equal(bstree<T, Options2...>&)
   template<class T, class ...Options2>
   void merge_equal(avltree<T, Options2...> &);

   friend bool operator< (const AvltreeImpl &x, const AvltreeImpl &y);

   friend bool operator==(const AvltreeImpl &x, const AvltreeImpl &y);

   friend bool operator!= (const AvltreeImpl &x, const AvltreeImpl &y);

   friend bool operator>(const AvltreeImpl &x, const AvltreeImpl &y);

   friend bool operator<=(const AvltreeImpl &x, const AvltreeImpl &y);

   friend bool operator>=(const AvltreeImpl &x, const AvltreeImpl &y);

   friend void swap(AvltreeImpl &x, AvltreeImpl &y);
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
};


//! Helper metafunction to define a \c avltree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeAvltree
{
   /// @cond
   typedef typename PackOptions
      < AvltreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef AvltreeImpl
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
class Avltree
   :  public MakeAvltree<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeAvltree
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Avltree)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::ReverseIterator           ReverseIterator;
   typedef typename Base::ConstReverseIterator     ConstReverseIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Avltree()
      :  Base()
   {}

   inline explicit Avltree( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline Avltree( bool unique, Iterator b, Iterator e
         , const KeyCompare &cmp = KeyCompare()
         , const ValueTraits &v_traits = ValueTraits())
      :  Base(unique, b, e, cmp, v_traits)
   {}

   inline Avltree(BOOST_RV_REF(Avltree) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Avltree& operator=(BOOST_RV_REF(Avltree) x)
   {  return static_cast<Avltree &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Avltree &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Avltree) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Avltree &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Avltree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Avltree &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Avltree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Avltree &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Avltree &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Avltree &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Avltree &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_AVLTREE_HPP
