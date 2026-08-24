/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2008-2013
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////
#ifndef BOOST_INTRUSIVE_TREAP_HPP
#define BOOST_INTRUSIVE_TREAP_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/bs_set_hook.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/detail/ebo_functor_holder.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/treap_algorithms.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/priority_compare.hpp>
#include <boost/intrusive/detail/node_cloner_disposer.hpp>
#include <boost/intrusive/detail/key_nodeptr_comp.hpp>

#include <boost/move/utility_core.hpp>
#include <boost/move/adl_move_swap.hpp>

#include <cstddef>
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>
#include <boost/intrusive/detail/minimal_pair_header.hpp>   //std::pair

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

struct TreapDefaults
   : BstreeDefaults
{
   typedef void Priority;
   typedef void PriorityOfValue;
};

template<class ValuePtr, class VoidOrPrioOfValue, class VoidOrPrioComp>
struct TreapPrioTypes
{
   typedef typename
      boost::movelib::pointer_element<ValuePtr>::type ValueType;
   typedef typename GetKeyOfValue
      < VoidOrPrioOfValue, ValueType>::type          PriorityOfValue;
   typedef typename PriorityOfValue::type           PriorityType;
   typedef typename GetPrioComp< VoidOrPrioComp
                      , PriorityType
                      >::type                         PriorityCompare;
};

struct treap_tag;

/// @endcond

//! The class template treap is an intrusive treap container that
//! is used to construct intrusive set and multiset containers. The no-throw
//! guarantee holds only, if the key_compare object and priority_compare object
//! don't throw.
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
class TreapImpl
   /// @cond
   : public BstreeImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType, ConstantTimeSize, BsTreeAlgorithmsValue, HeaderHolder>
   //Use public inheritance to avoid MSVC bugs with closures
   , public detail::EboFunctorHolder
         < typename TreapPrioTypes<typename TValueTraits::pointer, VoidOrPrioOfValue, VoidOrPrioComp>::priority_compare
         , treap_tag>
   /// @endcond
{
   public:
   typedef TValueTraits                                               ValueTraits;
   /// @cond
   typedef BstreeImpl< TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType
                      , ConstantTimeSize, BsTreeAlgorithmsValue
                      , HeaderHolder>                                TreeType;
   typedef TreeType                                                 ImplementationDefined;
   typedef TreapPrioTypes
      < typename TValueTraits::pointer
      , VoidOrPrioOfValue, VoidOrPrioComp>                           TreapPrioTypesT;

   typedef detail::EboFunctorHolder
      <typename TreapPrioTypesT::priority_compare, treap_tag>     PrioBase;

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
   typedef BOOST_INTRUSIVE_IMPDEF(TreapAlgorithms<NodeTraits>)     NodeAlgorithms;
   typedef BOOST_INTRUSIVE_IMPDEF
      (typename TreapPrioTypesT::PriorityType)                   PriorityType;
   typedef BOOST_INTRUSIVE_IMPDEF
      (typename TreapPrioTypesT::PriorityOfValue)               PriorityOfValue;
   typedef BOOST_INTRUSIVE_IMPDEF
      (typename TreapPrioTypesT::PriorityCompare)                PriorityCompare;

   static const bool CONSTANT_TIME_SIZE      = ImplementationDefined::CONSTANT_TIME_SIZE;
   static const bool STATEFUL_VALUE_TRAITS   = ImplementationDefined::STATEFUL_VALUE_TRAITS;
   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   typedef detail::KeyNodeptrComp<PriorityCompare, ValueTraits, PriorityOfValue> PrioNodePrioCompT;

   template<class PrioPrioComp>
   detail::KeyNodeptrComp<PrioPrioComp, ValueTraits, PriorityOfValue> prio_node_prio_comp(PrioPrioComp priopriocomp) const
   {  return detail::KeyNodeptrComp<PrioPrioComp, ValueTraits, PriorityOfValue>(priopriocomp, &this->get_value_traits());  }

   /// @cond
   private:

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(TreapImpl)

   const PriorityCompare &priv_pcomp() const
   {  return static_cast<const PrioBase&>(*this).get();  }

   PriorityCompare &priv_pcomp()
   {  return static_cast<PrioBase&>(*this).get();  }

   /// @endcond

   public:
   typedef typename NodeAlgorithms::InsertCommitData InsertCommitData;

   //! <b>Effects</b>: Constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare/priority_compare objects throw. Basic guarantee.
   TreapImpl()
      : TreeType(), PrioBase()
   {}

   //! <b>Effects</b>: Constructs an empty container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor of the value_compare/priority_compare objects throw. Basic guarantee.
   explicit TreapImpl( const KeyCompare &cmp
                      , const PriorityCompare &pcmp = PriorityCompare()
                      , const ValueTraits &v_traits = ValueTraits())
      : TreeType(cmp, v_traits), PrioBase(pcmp)
   {}

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!   cmp must be a comparison function that induces a strict weak ordering.
   //!
   //! <b>Effects</b>: Constructs an empty container and inserts elements from
   //!   [b, e).
   //!
   //! <b>Complexity</b>: Linear in N if [b, e) is already sorted using
   //!   comp and otherwise N * log N, where N is the distance between first and last.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the copy constructor/operator() of the key_compare/priority_compare objects
   //!   throw. Basic guarantee.
   template<class Iterator>
   TreapImpl( bool unique, Iterator b, Iterator e
            , const KeyCompare &cmp     = KeyCompare()
            , const PriorityCompare &pcmp = PriorityCompare()
            , const ValueTraits &v_traits = ValueTraits())
      : TreeType(cmp, v_traits), PrioBase(pcmp)
   {
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   TreapImpl(BOOST_RV_REF(TreapImpl) x)
      : TreeType(BOOST_MOVE_BASE(TreeType, x))
      , PrioBase(::boost::move(x.priv_pcomp()))
   {}

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   TreapImpl& operator=(BOOST_RV_REF(TreapImpl) x)
   {  this->swap(x); return *this;  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~TreapImpl();

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
   #endif

   //! <b>Effects</b>: Returns an iterator pointing to the highest priority object of the treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline Iterator top() BOOST_NOEXCEPT
   {  return this->TreeType::root();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the treap..
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline ConstIterator top() const BOOST_NOEXCEPT
   {  return this->ctop();   }

   //! <b>Effects</b>: Returns a const_iterator pointing to the highest priority object of the treap..
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline ConstIterator ctop() const BOOST_NOEXCEPT
   {  return this->TreeType::root();   }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
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

   #endif

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the highest priority object of the
   //!    reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline ReverseIterator rtop() BOOST_NOEXCEPT
   {  return ReverseIterator(this->top());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority objec
   //!    of the reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline ConstReverseIterator rtop() const BOOST_NOEXCEPT
   {  return ConstReverseIterator(this->top());  }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the highest priority object
   //!    of the reversed treap.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: Nothing.
   inline ConstReverseIterator crtop() const BOOST_NOEXCEPT
   {  return ConstReverseIterator(this->top());  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(iterator)
   static TreapImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const TreapImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static TreapImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const TreapImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const BOOST_NOEXCEPT;
   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Effects</b>: Returns the priority_compare object used by the container.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If priority_compare copy-constructor throws.
   PriorityCompare priority_comp() const
   {  return this->priv_pcomp();   }

   //! <b>Effects</b>: Swaps the contents of two treaps.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If the comparison functor's swap call throws.
   void swap(TreapImpl& other)
   {
      //This can throw
      ::boost::adl_move_swap(this->priv_pcomp(), other.priv_pcomp());
      TreeType::swap(other);
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const TreapImpl &src, Cloner cloner, Disposer disposer)
   {
      TreeType::clone_from(src, cloner, disposer);
      this->priv_pcomp() = src.priv_pcomp();
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(reference)
   //!   and inserts them on *this. Copies the predicate from the source container.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws or predicate copy assignment throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(TreapImpl) src, Cloner cloner, Disposer disposer)
   {
      TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);
      this->priv_pcomp() = ::boost::move(src.priv_pcomp());
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container before the upper bound.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_equal(Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      Iterator ret
         ( NodeAlgorithms::insert_equal_upper_bound
            ( this->TreeType::header_ptr()
            , to_insert
            , this->key_node_comp(this->key_comp())
            , this->prio_node_prio_comp(this->priv_pcomp()))
         , this->priv_value_traits_ptr());
      this->TreeType::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator.
   //!
   //! <b>Effects</b>: Inserts x into the container, using "hint" as a hint to
   //!   where it will be inserted. If "hint" is the upper_bound
   //!   the insertion takes constant time (two comparisons in the worst case)
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw. Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_equal(ConstIterator hint, Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      Iterator ret
         (NodeAlgorithms::insert_equal
            ( this->TreeType::header_ptr()
            , hint.pointed_node()
            , to_insert
            , this->key_node_comp(this->key_comp())
            , this->prio_node_prio_comp(this->priv_pcomp()))
         , this->priv_value_traits_ptr());
      this->TreeType::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Inserts a each element of a range into the container
   //!   before the upper bound of the key of each element.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by key_comp().
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      Iterator iend(this->end());
      for (; b != e; ++b)
         this->insert_equal(iend, *b);
   }

   //! <b>Requires</b>: value must be an lvalue
   //!
   //! <b>Effects</b>: Inserts value into the container if the value
   //!   is not already present.
   //!
   //! <b>Complexity</b>: Average complexity for insert element is at
   //!   most logarithmic.
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   std::pair<Iterator, bool> insert_unique(Reference value)
   {
      InsertCommitData commit_data;
      std::pair<Iterator, bool> ret = this->insert_unique_check(KeyOfValue()(value), PriorityOfValue()(value), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<Iterator, bool> (this->insert_unique_commit(value, commit_data), true);
   }

   //! <b>Requires</b>: value must be an lvalue, and "hint" must be
   //!   a valid iterator
   //!
   //! <b>Effects</b>: Tries to insert x into the container, using "hint" as a hint
   //!   to where it will be inserted.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it is amortized
   //!   constant time (two comparisons in the worst case)
   //!   if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   Iterator insert_unique(ConstIterator hint, Reference value)
   {
      InsertCommitData commit_data;
      std::pair<Iterator, bool> ret = this->insert_unique_check(hint, KeyOfValue()(value), PriorityOfValue()(value), commit_data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(value, commit_data);
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue
   //!   of type value_type.
   //!
   //! <b>Effects</b>: Tries to insert each element of a range into the container.
   //!
   //! <b>Complexity</b>: Insert range is in general O(N * log(N)), where N is the
   //!   size of the range. However, it is linear in N if the range is already sorted
   //!   by key_comp().
   //!
   //! <b>Throws</b>: If the internal key_compare or priority_compare functions throw.
   //!   Strong guarantee.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   No copy-constructors are called.
   template<class Iterator>
   void insert_unique(Iterator b, Iterator e)
   {
      if(this->empty()){
         Iterator iend(this->end());
         for (; b != e; ++b)
            this->insert_unique(iend, *b);
      }
      else{
         for (; b != e; ++b)
            this->insert_unique(*b);
      }
   }

   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the comparison or predicate functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that
   //!   part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   std::pair<Iterator, bool> insert_unique_check
      ( const KeyType &key, const PriorityType &prio, InsertCommitData &commit_data)
   {  return this->insert_unique_check(key, this->key_comp(), prio, this->priv_pcomp(), commit_data); }

   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint"
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the comparison or predicate functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   constructing that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that key
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This can give a total
   //!   constant-time complexity to the insertion: check(O(1)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   std::pair<Iterator, bool> insert_unique_check
      ( ConstIterator hint, const KeyType &key, const PriorityType &prio, InsertCommitData &commit_data)
   {  return this->insert_unique_check(hint, key, this->key_comp(), prio, this->priv_pcomp(), commit_data); }

   //! <b>Requires</b>: comp must be a comparison function that induces
   //!   the same strict weak ordering as key_compare.
   //!   prio_value_pcomp must be a comparison function that induces
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   prio_value_pcomp and comp compare an arbitrary key/priority with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Average complexity is at most logarithmic.
   //!
   //! <b>Throws</b>: If the comp or prio_value_pcomp
   //!   ordering functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   node that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that
   //!   part to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This gives a total
   //!   logarithmic complexity to the insertion: check(O(log(N)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyTypeKeyCompare, class PrioType, class PrioValuePrioCompare>
   BOOST_INTRUSIVE_DOC1ST(std::pair<iterator BOOST_INTRUSIVE_I bool>
      , typename detail::disable_if_convertible
         <KeyType BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I 
         std::pair<Iterator BOOST_INTRUSIVE_I bool> >::type)
      insert_unique_check
      ( const KeyType &key, KeyTypeKeyCompare comp
      , const PrioType &prio, PrioValuePrioCompare prio_value_pcomp, InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> const ret =
         (NodeAlgorithms::insert_unique_check
            ( this->TreeType::header_ptr()
            , key, this->key_node_comp(comp)
            , prio, this->prio_node_prio_comp(prio_value_pcomp)
            , commit_data));
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }

   //! <b>Requires</b>: comp must be a comparison function that induces
   //!   the same strict weak ordering as key_compare.
   //!   prio_value_pcomp must be a comparison function that induces
   //!   the same strict weak ordering as priority_compare. The difference is that
   //!   prio_value_pcomp and comp compare an arbitrary key/priority with the contained values.
   //!
   //! <b>Effects</b>: Checks if a value can be inserted in the container, using
   //!   a user provided key instead of the value itself, using "hint"
   //!   as a hint to where it will be inserted.
   //!
   //! <b>Returns</b>: If there is an equivalent value
   //!   returns a pair containing an iterator to the already present value
   //!   and false. If the value can be inserted returns true in the returned
   //!   pair boolean and fills "commit_data" that is meant to be used with
   //!   the "insert_commit" function.
   //!
   //! <b>Complexity</b>: Logarithmic in general, but it's amortized
   //!   constant time if t is inserted immediately before hint.
   //!
   //! <b>Throws</b>: If the comp or prio_value_pcomp
   //!   ordering functions throw. Strong guarantee.
   //!
   //! <b>Notes</b>: This function is used to improve performance when constructing
   //!   a value_type is expensive: if there is an equivalent value
   //!   the constructed object must be discarded. Many times, the part of the
   //!   constructing that is used to impose the order is much cheaper to construct
   //!   than the value_type and this function offers the possibility to use that key
   //!   to check if the insertion will be successful.
   //!
   //!   If the check is successful, the user can construct the value_type and use
   //!   "insert_commit" to insert the object in constant-time. This can give a total
   //!   constant-time complexity to the insertion: check(O(1)) + commit(O(1)).
   //!
   //!   "commit_data" remains valid for a subsequent "insert_commit" only if no more
   //!   objects are inserted or erased from the container.
   template<class KeyType, class KeyTypeKeyCompare, class PrioType, class PrioValuePrioCompare>
   std::pair<Iterator, bool> insert_unique_check
      ( ConstIterator hint
      , const KeyType &key
      , KeyTypeKeyCompare comp
      , const PrioType &prio
      , PrioValuePrioCompare prio_value_pcomp
      , InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> const ret =
         (NodeAlgorithms::insert_unique_check
            ( this->TreeType::header_ptr(), hint.pointed_node()
            , key, this->key_node_comp(comp)
            , prio, this->prio_node_prio_comp(prio_value_pcomp)
            , commit_data));
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }

   //! <b>Requires</b>: value must be an lvalue of type value_type. commit_data
   //!   must have been obtained from a previous call to "insert_check".
   //!   No objects should have been inserted or erased from the container between
   //!   the "insert_check" that filled "commit_data" and the call to "insert_commit".
   //!
   //! <b>Effects</b>: Inserts the value in the avl_set using the information obtained
   //!   from the "commit_data" that a previous "insert_check" filled.
   //!
   //! <b>Returns</b>: An iterator to the newly inserted object.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: Nothing
   //!
   //! <b>Notes</b>: This function has only sense if a "insert_check" has been
   //!   previously executed to fill "commit_data". No value should be inserted or
   //!   erased between the "insert_check" and "insert_commit" calls.
   Iterator insert_unique_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      NodeAlgorithms::insert_unique_commit(this->TreeType::header_ptr(), to_insert, commit_data);
      this->TreeType::sz_traits().increment();
      return Iterator(to_insert, this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be an lvalue, "pos" must be
   //!   a valid iterator (or end) and must be the succesor of value
   //!   once inserted according to the predicate
   //!
   //! <b>Effects</b>: Inserts x into the container before "pos".
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if "pos" is not
   //! the successor of "value" container ordering invariant will be broken.
   //! This is a low-level function to be used only for performance reasons
   //! by advanced users.
   Iterator insert_before(ConstIterator pos, Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      Iterator ret
         ( NodeAlgorithms::insert_before
            ( this->TreeType::header_ptr()
            , pos.pointed_node()
            , to_insert
            , this->prio_node_prio_comp(this->priv_pcomp())
            )
         , this->priv_value_traits_ptr());
      this->TreeType::sz_traits().increment();
      return ret;
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no less
   //!   than the greatest inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the last position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   less than the greatest inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_back(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      NodeAlgorithms::push_back
         (this->TreeType::header_ptr(), to_insert, this->prio_node_prio_comp(this->priv_pcomp()));
      this->TreeType::sz_traits().increment();
   }

   //! <b>Requires</b>: value must be an lvalue, and it must be no greater
   //!   than the minimum inserted key
   //!
   //! <b>Effects</b>: Inserts x into the container in the first position.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Throws</b>: If the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: This function does not check preconditions so if value is
   //!   greater than the minimum inserted key container ordering invariant will be broken.
   //!   This function is slightly more efficient than using "insert_before".
   //!   This is a low-level function to be used only for performance reasons
   //!   by advanced users.
   void push_front(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::unique(to_insert));
      NodeAlgorithms::push_front
         (this->TreeType::header_ptr(), to_insert, this->prio_node_prio_comp(this->priv_pcomp()));
      this->TreeType::sz_traits().increment();
   }

   //! <b>Effects</b>: Erases the element pointed to by i.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   Iterator erase(ConstIterator i) BOOST_NOEXCEPT
   {
      ConstIterator ret(i);
      ++ret;
      NodePtr to_erase(i.pointed_node());
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(to_erase));
      NodeAlgorithms::erase
         (this->TreeType::header_ptr(), to_erase, this->prio_node_prio_comp(this->priv_pcomp()));
      this->TreeType::sz_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      return ret.unconst();
   }

   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   Iterator erase(ConstIterator b, ConstIterator e) BOOST_NOEXCEPT
   {  SizeType n;   return private_erase(b, e, n);   }

   //! <b>Effects</b>: Erases all the elements with the given value.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   SizeType erase(const KeyType &key)
   {  return this->erase(key, this->key_comp());   }

   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws.
   //!   Equivalent guarantee to <i>while(beg != end) erase(beg++);</i>
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class KeyType, class KeyTypeKeyCompare>
   BOOST_INTRUSIVE_DOC1ST(size_type
      , typename detail::disable_if_convertible<KeyTypeKeyCompare BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I SizeType>::type)
      erase(const KeyType& key, KeyTypeKeyCompare comp)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key, comp);
      SizeType n;
      private_erase(p.first, p.second, n);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed to by i.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Complexity</b>: Average complexity for erase element is constant time.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator i, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr to_erase(i.pointed_node());
      Iterator ret(this->erase(i));
      disposer(this->get_value_traits().to_value_ptr(to_erase));
      return ret;
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   Iterator erase_and_dispose(Iterator i, Disposer disposer) BOOST_NOEXCEPT
   {  return this->erase_and_dispose(ConstIterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed to by b end e.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Complexity</b>: Average complexity for erase range is at most
   //!   O(log(size() + N)), where N is the number of elements in the range.
   //!
   //! <b>Throws</b>: if the internal priority_compare function throws. Strong guarantee.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator b, ConstIterator e, Disposer disposer) BOOST_NOEXCEPT
   {  SizeType n;   return private_erase(b, e, n, disposer);   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given value.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the priority_compare function throws then weak guarantee and heap invariants are broken.
   //!   The safest thing would be to clear or destroy the container.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. No destructors are called.
   template<class Disposer>
   SizeType erase_and_dispose(const KeyType &key, Disposer disposer)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key);
      SizeType n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements with the given key.
   //!   according to the comparison functor "comp".
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Complexity</b>: O(log(size() + N).
   //!
   //! <b>Throws</b>: if the priority_compare function throws then weak guarantee and heap invariants are broken.
   //!   The safest thing would be to clear or destroy the container.
   //!
   //! <b>Note</b>: Invalidates the iterators
   //!    to the erased elements.
   template<class KeyType, class KeyTypeKeyCompare, class Disposer>
   BOOST_INTRUSIVE_DOC1ST(size_type
      , typename detail::disable_if_convertible<KeyTypeKeyCompare BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I SizeType>::type)
      erase_and_dispose(const KeyType& key, KeyTypeKeyCompare comp, Disposer disposer)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key, comp);
      SizeType n;
      private_erase(p.first, p.second, n, disposer);
      return n;
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
   {  TreeType::clear(); }

   //! <b>Effects</b>: Erases all of the elements calling disposer(p) for
   //!   each node to be erased.
   //! <b>Complexity</b>: Average complexity for is at most O(log(size() + N)),
   //!   where N is the number of elements in the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!    to the erased elements. Calls N times to disposer functor.
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      NodeAlgorithms::clear_and_dispose(this->TreeType::header_ptr()
         , detail::NodeDisposer<Disposer, ValueTraits, TreapAlgorithmsValue>(disposer, &this->get_value_traits()));
      NodeAlgorithms::init_header(this->TreeType::header_ptr());
      this->TreeType::sz_traits().set_size(0);
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! @copydoc ::boost::intrusive::bstree::merge_unique
   template<class T, class ...Options2> void merge_unique(sgtree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_unique(TreapImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);

         if( NodeAlgorithms::transfer_unique
               ( this->header_ptr(), this->key_node_comp(this->key_comp())
               , this->prio_node_prio_comp(this->priv_pcomp()), source.header_ptr(), p) ){
            this->sz_traits().increment();
            source.sz_traits().decrement();
         }
      }
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! @copydoc ::boost::intrusive::bstree::merge_equal(bstree<T, Options2...>&)
   template<class T, class ...Options2> void merge_equal(sgtree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_equal(TreapImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, VoidOrPrioOfValue, VoidOrPrioComp, TSizeType, ConstantTimeSize, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);
         NodeAlgorithms::transfer_equal
            ( this->header_ptr(), this->key_node_comp(this->key_comp())
            , this->prio_node_prio_comp(this->priv_pcomp()), source.header_ptr(), p);
         this->sz_traits().increment();
         source.sz_traits().decrement();
      }
   }

   //! @copydoc ::boost::intrusive::bstree::check(ExtraChecker)const
   template <class ExtraChecker>
   void check(ExtraChecker extra_checker) const
   {
      typedef detail::KeyNodeptrComp<PriorityCompare, ValueTraits, PriorityOfValue> NodeptrPrioCompT;
      TreeType::check(detail::TreapNodeExtraChecker
         <TValueTraits, NodeptrPrioCompT, ExtraChecker>
            (this->prio_node_prio_comp(this->priv_pcomp()), extra_checker));
   }

   //! @copydoc ::boost::intrusive::bstree::check()const
   void check() const
   {  check(detail::EmptyNodeChecker<TValueTraits>());  }

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
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
      (const key_type &lower_key, const key_type &upper_key, bool left_closed, bool right_closed);

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

   friend bool operator< (const TreapImpl &x, const TreapImpl &y);

   friend bool operator==(const TreapImpl &x, const TreapImpl &y);

   friend bool operator!= (const TreapImpl &x, const TreapImpl &y);

   friend bool operator>(const TreapImpl &x, const TreapImpl &y);

   friend bool operator<=(const TreapImpl &x, const TreapImpl &y);

   friend bool operator>=(const TreapImpl &x, const TreapImpl &y);

   friend void swap(TreapImpl &x, TreapImpl &y);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   /// @cond
   private:
   template<class Disposer>
   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n, Disposer disposer)
   {
      for(n = 0; b != e; ++n)
        this->erase_and_dispose(b++, disposer);
      return b.unconst();
   }

   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n)
   {
      for(n = 0; b != e; ++n)
        this->erase(b++);
      return b.unconst();
   }
   /// @endcond
};


//! Helper metafunction to define a \c treap that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void
                , class O7 = void>
#endif
struct MakeTreap
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

   typedef TreapImpl
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
class Treap
   :  public MakeTreap<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeTreap
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6, O7
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Treap)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::PriorityCompare   PriorityCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::ReverseIterator           ReverseIterator;
   typedef typename Base::ConstReverseIterator     ConstReverseIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Treap()
      :  Base()
   {}

   inline explicit Treap( const KeyCompare &cmp
                 , const PriorityCompare &pcmp = PriorityCompare()
                 , const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, pcmp, v_traits)
   {}

   template<class Iterator>
   inline Treap( bool unique, Iterator b, Iterator e
       , const KeyCompare &cmp = KeyCompare()
       , const PriorityCompare &pcmp = PriorityCompare()
       , const ValueTraits &v_traits = ValueTraits())
      :  Base(unique, b, e, cmp, pcmp, v_traits)
   {}

   inline Treap(BOOST_RV_REF(Treap) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Treap& operator=(BOOST_RV_REF(Treap) x)
   {  return static_cast<Treap&>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Treap &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Treap) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Treap &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Treap &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Treap &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Treap &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Treap &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Treap &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Treap &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Treap &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_TREAP_HPP
