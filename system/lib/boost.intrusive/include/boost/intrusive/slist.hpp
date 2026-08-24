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

#ifndef BOOST_INTRUSIVE_SLIST_HPP
#define BOOST_INTRUSIVE_SLIST_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>

#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/slist_hook.hpp>
#include <boost/intrusive/circular_slist_algorithms.hpp>
#include <boost/intrusive/linear_slist_algorithms.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>
#include <boost/intrusive/detail/default_header_holder.hpp>
#include <boost/intrusive/detail/uncast.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/iterator.hpp>
#include <boost/intrusive/detail/slist_iterator.hpp>
#include <boost/intrusive/detail/array_initializer.hpp>
#include <boost/intrusive/detail/exception_disposer.hpp>
#include <boost/intrusive/detail/equal_to_value.hpp>
#include <boost/intrusive/detail/key_nodeptr_comp.hpp>
#include <boost/intrusive/detail/simple_disposers.hpp>
#include <boost/intrusive/detail/size_holder.hpp>
#include <boost/intrusive/detail/algorithm.hpp>
#include <boost/intrusive/detail/value_functors.hpp>
#include <boost/intrusive/detail/node_cloner_disposer.hpp>

#include <boost/move/utility_core.hpp>

#include <cstddef>   //std::size_t

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

template<class HeaderHolder, class NodePtr, bool>
struct HeaderHolderPlusLast
{
   HeaderHolder m_header_holder;
   NodePtr  m_last;
};

template<class HeaderHolder, class NodePtr>
struct HeaderHolderPlusLast<HeaderHolder, NodePtr, false>
{
   HeaderHolder m_header_holder;
};

struct DefaultSlistHookApplier
{  template <class T> struct Apply{ typedef typename T::default_slist_hook Type;  };  };

template<>
struct IsDefaultHookTag<DefaultSlistHookApplier>
{  static const bool VALUE = true;  };

struct SlistDefaults
{
   typedef DefaultSlistHookApplier ProtoValueTraits;
   static const bool CONSTANT_TIME_SIZE = true;
   static const bool LINEAR = false;
   typedef std::size_t SizeType;
   static const bool CACHE_LAST = false;
   typedef void HeaderHolderType;
};

struct SlistBoolFlags
{
   static const std::size_t LINEAR_POS             = 1u;
   static const std::size_t CONSTANT_TIME_SIZE_POS = 2u;
   static const std::size_t CACHE_LAST_POS         = 4u;
};


/// @endcond

//! The class template slist is an intrusive container, that encapsulates
//! a singly-linked list. You can use such a list to squeeze the last bit
//! of performance from your application. Unfortunately, the little gains
//! come with some huge drawbacks. A lot of member functions can't be
//! implemented as efficiently as for standard containers. To overcome
//! this limitation some other member functions with rather unusual semantics
//! have to be introduced.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<>, \c size_type<>,
//! \c linear<> and \c cache_last<>.
//!
//! The iterators of slist are forward iterators. slist provides a static
//! function called "previous" to compute the previous iterator of a given iterator.
//! This function has linear complexity. To improve the usability esp. with
//! the '*_after' functions, ++end() == begin() and previous(begin()) == end()
//! are defined. An new special function "before_begin()" is defined, which returns
//! an iterator that points one less the beginning of the list: ++before_begin() == begin()
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class TSizeType, std::size_t BoolFlags, typename HeaderHolder>
#endif
class SlistImpl
{
   //Public typedefs
   public:
   typedef TValueTraits                                               ValueTraits;
   typedef typename ValueTraits::Pointer                            Pointer;
   typedef typename ValueTraits::ConstPointer                      ConstPointer;
   typedef typename PointerTraits<Pointer>::element_type            ValueType;
   typedef typename PointerTraits<Pointer>::Reference               Reference;
   typedef typename PointerTraits<ConstPointer>::reference         ConstReference;
   typedef typename PointerTraits<Pointer>::DifferenceType         DifferenceType;
   typedef TSizeType                                                  SizeType;
   typedef SlistIterator<ValueTraits, false>                       Iterator;
   typedef SlistIterator<ValueTraits, true>                        ConstIterator;
   typedef typename ValueTraits::NodeTraits                        NodeTraits;
   typedef typename NodeTraits::Node                                Node;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef typename detail::GetHeaderHolderType
      < ValueTraits, HeaderHolder >::type                           HeaderHolderType;

   static const bool CONSTANT_TIME_SIZE = 0 != (BoolFlags & SlistBoolFlags::CONSTANT_TIME_SIZE_POS);
   static const bool STATEFUL_VALUE_TRAITS = detail::IsStatefulValueTraits<ValueTraits>::value;
   static const bool LINEAR = 0 != (BoolFlags & SlistBoolFlags::LINEAR_POS);
   static const bool CACHE_LAST = 0 != (BoolFlags & SlistBoolFlags::CACHE_LAST_POS);
   static const bool HAS_CONTAINER_FROM_ITERATOR =
        detail::is_same< HeaderHolderType, detail::DefaultHeaderHolder< NodeTraits > >::value;

   typedef typename detail::if_c
      < LINEAR
      , LinearSlistAlgorithms<NodeTraits>
      , CircularSlistAlgorithms<NodeTraits>
      >::type                                                        NodeAlgorithms;

   /// @cond
   private:
   typedef detail::SizeHolder<CONSTANT_TIME_SIZE, SizeType>        SizeTraits;

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(SlistImpl)

   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CONSTANT_TIME_SIZE && ((int)ValueTraits::link_mode == (int)AutoUnlink)));
   //Linear singly linked lists are incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(LINEAR && ((int)ValueTraits::link_mode == (int)AutoUnlink)));
   //A list with cached last node is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CACHE_LAST && ((int)ValueTraits::link_mode == (int)AutoUnlink)));

   inline NodePtr get_end_node()
   {  return NodeAlgorithms::end_node(this->get_root_node());  }

   inline ConstNodePtr get_end_node() const
   {  return NodeAlgorithms::end_node(this->get_root_node());  }

   inline NodePtr get_root_node()
   { return m_data.m_root_plus_size.header_holder_.get_node(); }

   inline ConstNodePtr get_root_node() const
   { return m_data.m_root_plus_size.header_holder_.get_node(); }

   inline NodePtr get_last_node()
   {  return this->get_last_node(detail::bool_<CACHE_LAST>());  }

   inline ConstNodePtr get_last_node() const
   {  return this->get_last_node(detail::bool_<CACHE_LAST>());  }

   inline void set_last_node(NodePtr n)
   {  return this->set_last_node(n, detail::bool_<CACHE_LAST>());  }

   inline static NodePtr get_last_node(detail::bool_<false>)
   {
      //This function shall not be used if cache_last is not true
      BOOST_INTRUSIVE_INVARIANT_ASSERT(CACHE_LAST);
      return NodePtr();
   }

   inline static void set_last_node(NodePtr , detail::bool_<false>)
   {
      //This function shall not be used if cache_last is not true
      BOOST_INTRUSIVE_INVARIANT_ASSERT(CACHE_LAST);
   }

   inline NodePtr get_last_node(detail::bool_<true>)
   {  return NodePtr(m_data.m_root_plus_size.last_);  }

   inline ConstNodePtr get_last_node(detail::bool_<true>) const
   {  return ConstNodePtr(m_data.m_root_plus_size.last_);  }

   inline void set_last_node(NodePtr n, detail::bool_<true>)
   {  m_data.m_root_plus_size.last_ = n;  }

   void set_default_constructed_state()
   {
      NodeAlgorithms::init_header(this->get_root_node());
      this->priv_size_traits().set_size(SizeType(0));
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         this->set_last_node(this->get_root_node());
      }
   }

   typedef HeaderHolderPlusLast<HeaderHolderType, NodePtr, CACHE_LAST> HeaderHolderPlusLastT;
   struct RootPlusSize
      :  public SizeTraits
      ,  public HeaderHolderPlusLastT
   {};

   struct DataT
      :  public ValueTraits
   {
      typedef typename SlistImpl::ValueTraits ValueTraits;
      explicit DataT(const ValueTraits &val_traits)
         :  ValueTraits(val_traits)
      {}

      RootPlusSize m_root_plus_size;
   } m_data;

   inline SizeTraits &priv_size_traits()
   {  return m_data.m_root_plus_size;  }

   inline const SizeTraits &priv_size_traits() const
   {  return m_data.m_root_plus_size;  }

   inline const ValueTraits &priv_value_traits() const
   {  return m_data;  }

   inline ValueTraits &priv_value_traits()
   {  return m_data;  }

   typedef typename boost::intrusive::ValueTraitsPointers
      <TValueTraits>::ConstValueTraitsPtr ConstValueTraitsPtr;

   inline ConstValueTraitsPtr priv_value_traits_ptr() const
   {  return PointerTraits<ConstValueTraitsPtr>::pointer_to(this->priv_value_traits());  }

   /// @endcond

   public:

   ///@cond

   //! <b>Requires</b>: f and before_l belong to another slist.
   //!
   //! <b>Effects</b>: Transfers the range [f, before_l] to this
   //!   list, after the element pointed by prev_pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred
   //!   if constant_time_size is true. Constant-time otherwise.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Warning</b>: Experimental function, don't use it!
   SlistImpl( NodePtr f, NodePtr before_l
             , SizeType n, const ValueTraits &v_traits = ValueTraits())
      :  m_data(v_traits)
   {
      if(n){
         this->priv_size_traits().set_size(n);
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            this->set_last_node(before_l);
         }
         NodeTraits::set_next(this->get_root_node(), f);
         NodeTraits::set_next(before_l, this->get_end_node());
      }
      else{
         this->set_default_constructed_state();
      }
   }

   ///@endcond

   //! <b>Effects</b>: constructs an empty list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   SlistImpl()
      :  m_data(ValueTraits())
   {  this->set_default_constructed_state(); }

   //! <b>Effects</b>: constructs an empty list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   explicit SlistImpl(const ValueTraits &v_traits)
      :  m_data(v_traits)
   {  this->set_default_constructed_state(); }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!
   //! <b>Effects</b>: Constructs a list equal to [b ,e).
   //!
   //! <b>Complexity</b>: Linear in distance(b, e). No copy constructors are called.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   template<class Iterator>
   SlistImpl(Iterator b, Iterator e, const ValueTraits &v_traits = ValueTraits())
      :  m_data(v_traits)
   {
      this->set_default_constructed_state();
      //nothrow, no need to rollback to release elements on exception
      this->insert_after(this->cbefore_begin(), b, e);
   }

   //! <b>Effects</b>: Constructs a container moving resources from another container.
   //!   Internal value traits are move constructed and
   //!   nodes belonging to x (except the node representing the "end") are linked to *this.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node's
   //!   move constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the move constructor of value traits throws.
   SlistImpl(BOOST_RV_REF(SlistImpl) x)
      : m_data(::boost::move(x.priv_value_traits()))
   {
      this->set_default_constructed_state();
      //nothrow, no need to rollback to release elements on exception
      this->swap(x);
   }

   //! <b>Effects</b>: Equivalent to swap
   //!
   SlistImpl& operator=(BOOST_RV_REF(SlistImpl) x)
   {  this->swap(x); return *this;  }

   //! <b>Effects</b>: If it's a safe-mode
   //!   or auto-unlink value, the destructor does nothing
   //!   (ie. no code is generated). Otherwise it detaches all elements from this.
   //!   In this case the objects in the list are not deleted (i.e. no destructors
   //!   are called), but the hooks according to the value_traits template parameter
   //!   are set to their default value.
   //!
   //! <b>Complexity</b>: Linear to the number of elements in the list, if
   //!   it's a safe-mode or auto-unlink value. Otherwise constant.
   ~SlistImpl()
   #if defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
      requires (TValueTraits::link_mode != NormalLink)
   #endif
   {
      BOOST_IF_CONSTEXPR(IsSafeAutounlink<TValueTraits::link_mode>::VALUE){
         this->clear();
         NodeAlgorithms::init(this->get_root_node());
      }
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) && defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
   //Default destructor for normal links (allows conditional triviality)
   ~SlistImpl() requires (TValueTraits::link_mode == NormalLink) = default;
   #endif

   //! <b>Effects</b>: Erases all the elements of the container.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements of the list.
   //!   if it's a safe-mode or auto-unlink value_type. Constant time otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the erased elements.
   void clear() BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
         this->clear_and_dispose(detail::NullDisposer());
      }
      else{
         this->set_default_constructed_state();
      }
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements of the container
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements of the list.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased elements.
   template <class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      NodeAlgorithms::detach_and_dispose
         ( this->get_root_node()
         , detail::NodeDisposer<Disposer, ValueTraits, CommonSListAlgorithmsValue>
               (disposer, &this->priv_value_traits()) );
      this->set_default_constructed_state();
   }

   //! <b>Requires</b>: value must be an lvalue.
   //!
   //! <b>Effects</b>: Inserts the value in the front of the list.
   //!   No copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   void push_front(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert = priv_value_traits().to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(to_insert));
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         if(this->empty()){
            this->set_last_node(to_insert);
         }
      }
      NodeAlgorithms::link_after(this->get_root_node(), to_insert);
      this->priv_size_traits().increment();
   }

   //! <b>Requires</b>: value must be an lvalue.
   //!
   //! <b>Effects</b>: Inserts the value in the back of the list.
   //!   No copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   This function is only available is cache_last<> is true.
   void push_back(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((CACHE_LAST));
      NodePtr n = priv_value_traits().to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(n));
      NodeAlgorithms::link_after(this->get_last_node(), n);
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         this->set_last_node(n);
      }
      this->priv_size_traits().increment();
   }

   //! <b>Effects</b>: Erases the first element of the list.
   //!   No destructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the erased element.
   void pop_front() BOOST_NOEXCEPT
   {  return this->pop_front_and_dispose(detail::NullDisposer());   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the first element of the list.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased element.
   template<class Disposer>
   void pop_front_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr to_erase = NodeTraits::get_next(this->get_root_node());
      NodeAlgorithms::unlink_after(this->get_root_node());
      this->priv_size_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(priv_value_traits().to_value_ptr(to_erase));
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         if(this->empty()){
            this->set_last_node(this->get_root_node());
         }
      }
   }

   //! <b>Effects</b>: Returns a reference to the first element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Reference front() BOOST_NOEXCEPT
   { return *this->priv_value_traits().to_value_ptr(NodeTraits::get_next(this->get_root_node())); }

   //! <b>Effects</b>: Returns a const_reference to the first element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReference front() const BOOST_NOEXCEPT
   { return *this->priv_value_traits().to_value_ptr(detail::uncast(NodeTraits::get_next(this->get_root_node()))); }

   //! <b>Effects</b>: Returns a reference to the last element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   This function is only available is cache_last<> is true.
   Reference back() BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((CACHE_LAST));
      return *this->priv_value_traits().to_value_ptr(this->get_last_node());
   }

   //! <b>Effects</b>: Returns a const_reference to the last element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   //!   This function is only available is cache_last<> is true.
   inline ConstReference back() const BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((CACHE_LAST));
      return *this->priv_value_traits().to_value_ptr(this->get_last_node());
   }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Iterator begin() BOOST_NOEXCEPT
   { return Iterator (NodeTraits::get_next(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator begin() const BOOST_NOEXCEPT
   { return ConstIterator (NodeTraits::get_next(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator cbegin() const BOOST_NOEXCEPT
   { return ConstIterator(NodeTraits::get_next(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns an iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Iterator end() BOOST_NOEXCEPT
   { return Iterator(this->get_end_node(), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator end() const BOOST_NOEXCEPT
   { return ConstIterator(detail::uncast(this->get_end_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator cend() const BOOST_NOEXCEPT
   { return this->end(); }

   //! <b>Effects</b>: Returns an iterator that points to a position
   //!   before the first element. Equivalent to "end()"
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Iterator before_begin() BOOST_NOEXCEPT
   { return Iterator(this->get_root_node(), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns an iterator that points to a position
   //!   before the first element. Equivalent to "end()"
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator before_begin() const BOOST_NOEXCEPT
   { return ConstIterator(detail::uncast(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns an iterator that points to a position
   //!   before the first element. Equivalent to "end()"
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator cbefore_begin() const BOOST_NOEXCEPT
   { return this->before_begin(); }

   //! <b>Effects</b>: Returns an iterator to the last element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: This function is present only if cached_last<> option is true.
   inline Iterator last() BOOST_NOEXCEPT
   {
      //This function shall not be used if cache_last is not true
      BOOST_INTRUSIVE_INVARIANT_ASSERT(CACHE_LAST);
      return Iterator (this->get_last_node(), this->priv_value_traits_ptr());
   }

   //! <b>Effects</b>: Returns a const_iterator to the last element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: This function is present only if cached_last<> option is true.
   inline ConstIterator last() const BOOST_NOEXCEPT
   {
      //This function shall not be used if cache_last is not true
      BOOST_INTRUSIVE_INVARIANT_ASSERT(CACHE_LAST);
      return ConstIterator (this->get_last_node(), this->priv_value_traits_ptr());
   }

   //! <b>Effects</b>: Returns a const_iterator to the last element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: This function is present only if cached_last<> option is true.
   inline ConstIterator clast() const BOOST_NOEXCEPT
   { return ConstIterator(this->get_last_node(), this->priv_value_traits_ptr()); }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of slist.
   //!
   //! <b>Effects</b>: Returns a const reference to the slist associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   inline static SlistImpl &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return SlistImpl::priv_container_from_end_iterator(end_iterator);   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of slist.
   //!
   //! <b>Effects</b>: Returns a const reference to the slist associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   inline static const SlistImpl &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return SlistImpl::priv_container_from_end_iterator(end_iterator);   }

   //! <b>Effects</b>: Returns the number of the elements contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements contained in the list.
   //!   if constant_time_size is false. Constant time otherwise.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   inline SizeType size() const BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         return this->priv_size_traits().get_size();
      else
         return NodeAlgorithms::count(this->get_root_node()) - 1;
   }

   //! <b>Effects</b>: Returns true if the list contains no elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   [[nodiscard]] inline bool empty() const BOOST_NOEXCEPT
   {  return NodeAlgorithms::is_empty(this->get_root_node()); }

   //! <b>Effects</b>: Swaps the elements of x and *this.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements of both lists.
   //!  Constant-time if linear<> and/or cache_last<> options are used.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   void swap(SlistImpl& other)
   {
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         priv_swap_cache_last(this, &other);
      }
      else{
         this->priv_swap_lists(this->get_root_node(), other.get_root_node(), detail::bool_<LINEAR>());
      }
      this->priv_size_traits().swap(other.priv_size_traits());
   }

   //! <b>Effects</b>: Moves backwards all the elements, so that the first
   //!   element becomes the second, the second becomes the third...
   //!   the last element becomes the first one.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements plus the number shifts.
   //!
   //! <b>Note</b>: Iterators Does not affect the validity of iterators and references.
   void shift_backwards(SizeType n = 1) BOOST_NOEXCEPT
   {  this->priv_shift_backwards(n, detail::bool_<LINEAR>());  }

   //! <b>Effects</b>: Moves forward all the elements, so that the second
   //!   element becomes the first, the third becomes the second...
   //!   the first element becomes the last one.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements plus the number shifts.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   void shift_forward(SizeType n = 1) BOOST_NOEXCEPT
   {  this->priv_shift_forward(n, detail::bool_<LINEAR>()); }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(const_reference )
   //!   and inserts them on *this.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws.
   template <class Cloner, class Disposer>
   void clone_from(const SlistImpl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      detail::ExceptionDisposer<SlistImpl, Disposer>
         rollback(*this, disposer);
      ConstIterator prev(this->cbefore_begin());
      ConstIterator b(src.begin()), e(src.end());
      for(; b != e; ++b){
         prev = this->insert_after(prev, *cloner(*b));
      }
      rollback.release();
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!   Cloner should yield to nodes equivalent to the original nodes.
   //!
   //! <b>Effects</b>: Erases all the elements from *this
   //!   calling Disposer::operator()(pointer), clones all the
   //!   elements from src calling Cloner::operator()(reference)
   //!   and inserts them on *this.
   //!
   //!   If cloner throws, all cloned elements are unlinked and disposed
   //!   calling Disposer::operator()(pointer).
   //!
   //! <b>Complexity</b>: Linear to erased plus inserted elements.
   //!
   //! <b>Throws</b>: If cloner throws.
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(SlistImpl) src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      detail::ExceptionDisposer<SlistImpl, Disposer>
         rollback(*this, disposer);
      Iterator prev(this->cbefore_begin());
      Iterator b(src.begin()), e(src.end());
      for(; b != e; ++b){
         prev = this->insert_after(prev, *cloner(*b));
      }
      rollback.release();
   }

   //! <b>Requires</b>: value must be an lvalue and prev_p must point to an element
   //!   contained by the list or to end().
   //!
   //! <b>Effects</b>: Inserts the value after the position pointed by prev_p.
   //!    No copy constructor is called.
   //!
   //! <b>Returns</b>: An iterator to the inserted element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   Iterator insert_after(ConstIterator prev_p, Reference value) BOOST_NOEXCEPT
   {
      NodePtr n = priv_value_traits().to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(n));
      NodePtr prev_n(prev_p.pointed_node());
      NodeAlgorithms::link_after(prev_n, n);

      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(this->get_last_node() == prev_n){
         this->set_last_node(n);
      }
      this->priv_size_traits().increment();
      return Iterator (n, this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: Dereferencing iterator must yield
   //!   an lvalue of type value_type and prev_p must point to an element
   //!   contained by the list or to the end node.
   //!
   //! <b>Effects</b>: Inserts the [f, l)
   //!   after the position prev_p.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements inserted.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   template<class Iterator>
   void insert_after(ConstIterator prev_p, Iterator f, Iterator l) BOOST_NOEXCEPT
   {
      //Insert first nodes avoiding cache and size checks
      SizeType count = 0;
      NodePtr prev_n(prev_p.pointed_node());
      for (; f != l; ++f, ++count){
         const NodePtr n = priv_value_traits().to_node_ptr(*f);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(n));
         NodeAlgorithms::link_after(prev_n, n);
         prev_n = n;
      }
      //Now fix special cases if needed
      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(this->get_last_node() == prev_p.pointed_node()){
         this->set_last_node(prev_n);
      }
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
         this->priv_size_traits().increase(count);
      }
   }

   //! <b>Requires</b>: value must be an lvalue and p must point to an element
   //!   contained by the list or to end().
   //!
   //! <b>Effects</b>: Inserts the value before the position pointed by p.
   //!   No copy constructor is called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements before p.
   //!  Constant-time if cache_last<> is true and p == end().
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   Iterator insert(ConstIterator p, Reference value) BOOST_NOEXCEPT
   {  return this->insert_after(this->previous(p), value);  }

   //! <b>Requires</b>: Dereferencing iterator must yield
   //!   an lvalue of type value_type and p must point to an element
   //!   contained by the list or to the end node.
   //!
   //! <b>Effects</b>: Inserts the pointed by b and e
   //!   before the position p. No copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements inserted plus linear
   //!   to the elements before b.
   //!   Linear to the number of elements to insert if cache_last<> option is true and p == end().
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   template<class Iterator>
   void insert(ConstIterator p, Iterator b, Iterator e) BOOST_NOEXCEPT
   {  return this->insert_after(this->previous(p), b, e);  }

   //! <b>Effects</b>: Erases the element after the element pointed by prev of
   //!   the list. No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   Iterator erase_after(ConstIterator prev) BOOST_NOEXCEPT
   {  return this->erase_after_and_dispose(prev, detail::NullDisposer());  }

   //! <b>Effects</b>: Erases the range (before_f, l) from
   //!   the list. No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of erased elements if it's a safe-mode
   //!   , auto-unlink value or constant-time size is activated. Constant time otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   Iterator erase_after(ConstIterator before_f, ConstIterator l) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK || CONSTANT_TIME_SIZE){
         return this->erase_after_and_dispose(before_f, l, detail::NullDisposer());
      }
      else{
         const NodePtr bfp = before_f.pointed_node();
         const NodePtr lp = l.pointed_node();
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            if(lp == this->get_end_node()){
               this->set_last_node(bfp);
            }
         }
         NodeAlgorithms::unlink_after(bfp, lp);
         return l.unconst();
      }
   }

   //! <b>Effects</b>: Erases the range (before_f, l) from
   //!   the list. n must be distance(before_f, l) - 1.
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: constant-time if link_mode is NormalLink.
   //!   Linear to the elements (l - before_f) otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   Iterator erase_after(ConstIterator before_f, ConstIterator l, SizeType n) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeAlgorithms::distance((++ConstIterator(before_f)).pointed_node(), l.pointed_node()) == n);
      (void)n;
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
         return this->erase_after(before_f, l);
      }
      else{
         const NodePtr bfp = before_f.pointed_node();
         const NodePtr lp = l.pointed_node();
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            if((lp == this->get_end_node())){
               this->set_last_node(bfp);
            }
         }
         NodeAlgorithms::unlink_after(bfp, lp);
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
            this->priv_size_traits().decrease(n);
         }
         return l.unconst();
      }
   }

   //! <b>Effects</b>: Erases the element pointed by i of the list.
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed element,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements before i.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   Iterator erase(ConstIterator i) BOOST_NOEXCEPT
   {  return this->erase_after(this->previous(i));  }

   //! <b>Requires</b>: f and l must be valid iterator to elements in *this.
   //!
   //! <b>Effects</b>: Erases the range pointed by b and e.
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements before l.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased elements.
   Iterator erase(ConstIterator f, ConstIterator l) BOOST_NOEXCEPT
   {  return this->erase_after(this->previous(f), l);  }

   //! <b>Effects</b>: Erases the range [f, l) from
   //!   the list. n must be distance(f, l).
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: linear to the elements before f if link_mode is NormalLink
   //!   and constant_time_size is activated. Linear to the elements before l otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   Iterator erase(ConstIterator f, ConstIterator l, SizeType n) BOOST_NOEXCEPT
   {  return this->erase_after(this->previous(f), l, n);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element after the element pointed by prev of
   //!   the list.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased element.
   template<class Disposer>
   Iterator erase_after_and_dispose(ConstIterator prev, Disposer disposer) BOOST_NOEXCEPT
   {
      ConstIterator it(prev);
      ++it;
      NodePtr to_erase(it.pointed_node());
      ++it;
      NodePtr prev_n(prev.pointed_node());
      NodeAlgorithms::unlink_after(prev_n);

      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(to_erase == this->get_last_node()){
         this->set_last_node(prev_n);
      }
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(priv_value_traits().to_value_ptr(to_erase));
      this->priv_size_traits().decrement();
      return it.unconst();
   }

   /// @cond

   static Iterator s_insert_after(ConstIterator const prev_p, Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT(((!CACHE_LAST)&&(!CONSTANT_TIME_SIZE)&&(!STATEFUL_VALUE_TRAITS)));
      NodePtr const n = ValueTraits::to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(n));
      NodeAlgorithms::link_after(prev_p.pointed_node(), n);
      return Iterator (n, ConstValueTraitsPtr());
   }

   template<class Disposer>
   static Iterator s_erase_after_and_dispose(ConstIterator prev, Disposer disposer) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT(((!CACHE_LAST)&&(!CONSTANT_TIME_SIZE)&&(!STATEFUL_VALUE_TRAITS)));
      ConstIterator it(prev);
      ++it;
      NodePtr to_erase(it.pointed_node());
      ++it;
      NodePtr prev_n(prev.pointed_node());
      NodeAlgorithms::unlink_after(prev_n);
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(ValueTraits::to_value_ptr(to_erase));
      return it.unconst();
   }

   template<class Disposer>
   static Iterator s_erase_after_and_dispose(ConstIterator before_f, ConstIterator l, Disposer disposer) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT(((!CACHE_LAST)&&(!CONSTANT_TIME_SIZE)&&(!STATEFUL_VALUE_TRAITS)));
      NodePtr bfp(before_f.pointed_node()), lp(l.pointed_node());
      NodePtr fp(NodeTraits::get_next(bfp));
      NodeAlgorithms::unlink_after(bfp, lp);
      while(fp != lp){
         NodePtr to_erase(fp);
         fp = NodeTraits::get_next(fp);
         BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
            NodeAlgorithms::init(to_erase);
         disposer(ValueTraits::to_value_ptr(to_erase));
      }
      return l.unconst();
   }

   static Iterator s_erase_after(ConstIterator prev) BOOST_NOEXCEPT
   {  return s_erase_after_and_dispose(prev, detail::NullDisposer());  }

   /// @endcond

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range (before_f, l) from
   //!   the list.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements (l - before_f + 1).
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased element.
   template<class Disposer>
   Iterator erase_after_and_dispose(ConstIterator before_f, ConstIterator l, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr bfp(before_f.pointed_node()), lp(l.pointed_node());
      NodePtr fp(NodeTraits::get_next(bfp));
      NodeAlgorithms::unlink_after(bfp, lp);
      while(fp != lp){
         NodePtr to_erase(fp);
         fp = NodeTraits::get_next(fp);
         BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
            NodeAlgorithms::init(to_erase);
         disposer(priv_value_traits().to_value_ptr(to_erase));
         this->priv_size_traits().decrement();
      }

      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(NodeTraits::get_next(bfp) == this->get_end_node()){
         this->set_last_node(bfp);
      }
      return l.unconst();
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element pointed by i of the list.
   //!   No destructors are called.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed element,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements before i.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator i, Disposer disposer) BOOST_NOEXCEPT
   {  return this->erase_after_and_dispose(this->previous(i), disposer);  }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   Iterator erase_and_dispose(Iterator i, Disposer disposer) BOOST_NOEXCEPT
   {  return this->erase_and_dispose(ConstIterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: f and l must be valid iterator to elements in *this.
   //!                  Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the range pointed by b and e.
   //!   No destructors are called.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of erased elements plus linear
   //!   to the elements before f.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased elements.
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator f, ConstIterator l, Disposer disposer) BOOST_NOEXCEPT
   {  return this->erase_after_and_dispose(this->previous(f), l, disposer);  }

   //! <b>Requires</b>: Dereferencing iterator must yield
   //!   an lvalue of type value_type.
   //!
   //! <b>Effects</b>: Clears the list and inserts the range pointed by b and e.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements inserted plus
   //!   linear to the elements contained in the list if it's a safe-mode
   //!   or auto-unlink value.
   //!   Linear to the number of elements inserted in the list otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!   to the erased elements.
   template<class Iterator>
   void assign(Iterator b, Iterator e)
   {
      this->clear();
      this->insert_after(this->cbefore_begin(), b, e);
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Requires</b>: Dereferencing iterator must yield
   //!   an lvalue of type value_type.
   //!
   //! <b>Effects</b>: Clears the list and inserts the range pointed by b and e.
   //!   No destructors or copy constructors are called.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements inserted plus
   //!   linear to the elements contained in the list.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references)
   //!   to the erased elements.
   template<class Iterator, class Disposer>
   void dispose_and_assign(Disposer disposer, Iterator b, Iterator e)
   {
      this->clear_and_dispose(disposer);
      this->insert_after(this->cbefore_begin(), b, e, disposer);
   }

   //! <b>Requires</b>: prev must point to an element contained by this list or
   //!   to the before_begin() element
   //!
   //! <b>Effects</b>: Transfers all the elements of list x to this list, after the
   //! the element pointed by prev. No destructors or copy constructors are called.
   //!
   //! <b>Returns</b>: Nothing.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: In general, linear to the elements contained in x.
   //!   Constant-time if cache_last<> option is true and also constant-time if
   //!   linear<> option is true "this" is empty and "l" is not used.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //! list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Additional note</b>: If the optional parameter "l" is provided, it will be
   //!   assigned to the last spliced element or prev if x is empty.
   //!   This iterator can be used as new "prev" iterator for a new splice_after call.
   //!   that will splice new values after the previously spliced values.
   void splice_after(ConstIterator prev, SlistImpl &x, ConstIterator *l = 0) BOOST_NOEXCEPT
   {
      if(x.empty()){
         if(l) *l = prev;
      }
      else if(LINEAR && this->empty()){
         this->swap(x);
         if(l) *l = this->previous(this->cend());
      }
      else{
         ConstIterator last_x(x.previous(x.end()));  //constant time if cache_last is active
         NodePtr prev_n(prev.pointed_node());
         NodePtr last_x_n(last_x.pointed_node());
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            x.set_last_node(x.get_root_node());
            if(NodeTraits::get_next(prev_n) == this->get_end_node()){
               this->set_last_node(last_x_n);
            }
         }
         NodeAlgorithms::transfer_after( prev_n, x.before_begin().pointed_node(), last_x_n);
         this->priv_size_traits().increase(x.priv_size_traits().get_size());
         x.priv_size_traits().set_size(SizeType(0));
         if(l) *l = last_x;
      }
   }

   //! <b>Requires</b>: prev must point to an element contained by this list or
   //!   to the before_begin() element. prev_ele must point to an element contained in list
   //!   x or must be x.before_begin().
   //!
   //! <b>Effects</b>: Transfers the element after prev_ele, from list x to this list,
   //!   after the element pointed by prev. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //! list. Iterators of this list and all the references are not invalidated.
   void splice_after(ConstIterator prev_pos, SlistImpl &x, ConstIterator prev_ele) BOOST_NOEXCEPT
   {
      ConstIterator elem = prev_ele;
      this->splice_after(prev_pos, x, prev_ele, ++elem, 1);
   }

   //! <b>Requires</b>: prev_pos must be a dereferenceable iterator in *this or be
   //!   before_begin(), and before_f and before_l belong to x and
   //!   ++before_f != x.end() && before_l != x.end().
   //!
   //! <b>Effects</b>: Transfers the range (before_f, before_l] from list x to this
   //!   list, after the element pointed by prev_pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred
   //!   if constant_time_size is true. Constant-time otherwise.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice_after(ConstIterator prev_pos, SlistImpl &x, ConstIterator before_f, ConstIterator before_l) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         this->splice_after(prev_pos, x, before_f, before_l, NodeAlgorithms::distance(before_f.pointed_node(), before_l.pointed_node()));
      else
         this->priv_splice_after
            (prev_pos.pointed_node(), x, before_f.pointed_node(), before_l.pointed_node());
   }

   //! <b>Requires</b>: prev_pos must be a dereferenceable iterator in *this or be
   //!   before_begin(), and before_f and before_l belong to x and
   //!   ++before_f != x.end() && before_l != x.end() and
   //!   n == distance(before_f, before_l).
   //!
   //! <b>Effects</b>: Transfers the range (before_f, before_l] from list x to this
   //!   list, after the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice_after(ConstIterator prev_pos, SlistImpl &x, ConstIterator before_f, ConstIterator before_l, SizeType n) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeAlgorithms::distance(before_f.pointed_node(), before_l.pointed_node()) == n);
      (void)n;
      this->priv_splice_after
         (prev_pos.pointed_node(), x, before_f.pointed_node(), before_l.pointed_node());
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
         this->priv_size_traits().increase(n);
         x.priv_size_traits().decrease(n);
      }
   }

   //! <b>Requires</b>: it is an iterator to an element in *this.
   //!
   //! <b>Effects</b>: Transfers all the elements of list x to this list, before the
   //! the element pointed by it. No destructors or copy constructors are called.
   //!
   //! <b>Returns</b>: Nothing.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements contained in x plus linear to
   //!   the elements before it.
   //!   Linear to the elements before it if cache_last<> option is true.
   //!   Constant-time if cache_last<> option is true and it == end().
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //! list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Additional note</b>: If the optional parameter "l" is provided, it will be
   //!   assigned to the last spliced element or prev if x is empty.
   //!   This iterator can be used as new "prev" iterator for a new splice_after call.
   //!   that will splice new values after the previously spliced values.
   void splice(ConstIterator it, SlistImpl &x, ConstIterator *l = 0) BOOST_NOEXCEPT
   {  this->splice_after(this->previous(it), x, l);   }

   //! <b>Requires</b>: it p must be a valid iterator of *this.
   //!   elem must point to an element contained in list
   //!   x.
   //!
   //! <b>Effects</b>: Transfers the element elem, from list x to this list,
   //!   before the element pointed by pos. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements before pos and before elem.
   //!   Linear to the elements before elem if cache_last<> option is true and pos == end().
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //! list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator pos, SlistImpl &x, ConstIterator elem) BOOST_NOEXCEPT
   {  return this->splice_after(this->previous(pos), x, x.previous(elem));  }

   //! <b>Requires</b>: pos must be a dereferenceable iterator in *this
   //!   and f and f belong to x and f and f a valid range on x.
   //!
   //! <b>Effects</b>: Transfers the range [f, l) from list x to this
   //!   list, before the element pointed by pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the sum of elements before pos, f, and l
   //!   plus linear to the number of elements transferred if constant_time_size is true.
   //!   Linear to the sum of elements before f, and l
   //!   plus linear to the number of elements transferred if constant_time_size is true
   //!   if cache_last<> is true and pos == end()
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator pos, SlistImpl &x, ConstIterator f, ConstIterator l) BOOST_NOEXCEPT
   {  return this->splice_after(this->previous(pos), x, x.previous(f), x.previous(l));  }

   //! <b>Requires</b>: pos must be a dereferenceable iterator in *this
   //!   and f and l belong to x and f and l a valid range on x.
   //!   n == distance(f, l).
   //!
   //! <b>Effects</b>: Transfers the range [f, l) from list x to this
   //!   list, before the element pointed by pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the sum of elements before pos, f, and l.
   //!   Linear to the sum of elements before f and l
   //!   if cache_last<> is true and pos == end().
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator pos, SlistImpl &x, ConstIterator f, ConstIterator l, SizeType n) BOOST_NOEXCEPT
   {  return this->splice_after(this->previous(pos), x, x.previous(f), x.previous(l), n);  }

   //! <b>Effects</b>: This function sorts the list *this according to operator<.
   //!   The sort is stable, that is, the relative order of equivalent elements is preserved.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the predicate throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: The number of comparisons is approximately N log N, where N
   //!   is the list's size.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   template<class Predicate>
   void sort(Predicate p)
   {
      if (NodeTraits::get_next(NodeTraits::get_next(this->get_root_node()))
         != this->get_root_node()) {

         SlistImpl carry(this->priv_value_traits());
         detail::ArrayInitializer<SlistImpl, 64> counter(this->priv_value_traits());
         int fill = 0;
         ConstIterator last_inserted;
         while(!this->empty()){
            last_inserted = this->cbegin();
            carry.splice_after(carry.cbefore_begin(), *this, this->cbefore_begin());
            int i = 0;
            while(i < fill && !counter[i].empty()) {
               carry.swap(counter[i]);
               carry.merge(counter[i++], p, &last_inserted);
            }
            BOOST_INTRUSIVE_INVARIANT_ASSERT(counter[i].empty());
            ConstIterator last_element(carry.previous(last_inserted, carry.end()));

            BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
               counter[i].splice_after( counter[i].cbefore_begin(), carry
                                    , carry.cbefore_begin(), last_element
                                    , carry.size());
            }
            else{
               counter[i].splice_after( counter[i].cbefore_begin(), carry
                                    , carry.cbefore_begin(), last_element);
            }
            if(i == fill)
               ++fill;
         }

         for (int i = 1; i < fill; ++i)
            counter[i].merge(counter[i-1], p, &last_inserted);
         --fill;
         ConstIterator last_element(counter[fill].previous(last_inserted, counter[fill].end()));
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
            this->splice_after( cbefore_begin(), counter[fill], counter[fill].cbefore_begin()
                              , last_element, counter[fill].size());
         }
         else{
            this->splice_after( cbefore_begin(), counter[fill], counter[fill].cbefore_begin()
                              , last_element);
         }
      }
   }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak
   //!   ordering and both *this and x must be sorted according to that ordering
   //!   The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this. The merge is stable; that is, if an element from *this is
   //!   equivalent to one from x, then the element from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or operator< throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   void sort()
   { this->sort(ValueLess<ValueType>()); }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak
   //!   ordering and both *this and x must be sorted according to that ordering
   //!   The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this. The merge is stable; that is, if an element from *this is
   //!   equivalent to one from x, then the element from *this will precede the one from x.
   //!
   //! <b>Returns</b>: Nothing.
   //!
   //! <b>Throws</b>: If the predicate throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   //!
   //! <b>Additional note</b>: If optional "l" argument is passed, it is assigned
   //! to an iterator to the last transferred value or end() is x is empty.
   template<class Predicate>
   void merge(SlistImpl& x, Predicate p, ConstIterator *l = 0)
   {
      ConstIterator e(this->cend()), ex(x.cend()), bb(this->cbefore_begin()),
                     bb_next;
      if(l) *l = e.unconst();
      while(!x.empty()){
         ConstIterator ibx_next(x.cbefore_begin()), ibx(ibx_next++);
         while (++(bb_next = bb) != e && !p(*ibx_next, *bb_next)){
            bb = bb_next;
         }
         if(bb_next == e){
            //Now transfer the rest to the end of the container
            this->splice_after(bb, x, l);
            break;
         }
         else{
            SizeType n(0);
            do{
               ibx = ibx_next; ++n;
            } while(++(ibx_next = ibx) != ex && p(*ibx_next, *bb_next));
            this->splice_after(bb, x, x.before_begin(), ibx, n);
            if(l) *l = ibx;
         }
      }
   }

   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this according to operator<. The merge is stable;
   //!   that is, if an element from *this is equivalent to one from x, then the element
   //!   from *this will precede the one from x.
   //!
   //! <b>Throws</b>: if operator< throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   void merge(SlistImpl& x)
   {  this->merge(x, ValueLess<ValueType>());  }

   //! <b>Effects</b>: Reverses the order of elements in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: This function is linear to the contained elements.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   void reverse() BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(!this->empty()){
         this->set_last_node(NodeTraits::get_next(this->get_root_node()));
      }
      this->priv_reverse(detail::bool_<LINEAR>());
   }

   //! <b>Effects</b>: Removes all the elements that compare equal to value.
   //!   No destructors are called.
   //!
   //! <b>Throws</b>: If operator== throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() comparisons for equality.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid. This function is
   //!   linear time: it performs exactly size() comparisons for equality.
   SizeType remove(ConstReference value) BOOST_NOEXCEPT
   {  return this->remove_if(detail::EqualToValue<ConstReference>(value));  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes all the elements that compare equal to value.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //!
   //! <b>Throws</b>: If operator== throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() comparisons for equality.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class Disposer>
   SizeType remove_and_dispose(ConstReference value, Disposer disposer) BOOST_NOEXCEPT
   {  return this->remove_and_dispose_if(detail::EqualToValue<ConstReference>(value), disposer);  }

   //! <b>Effects</b>: Removes all the elements for which a specified
   //!   predicate is satisfied. No destructors are called.
   //!
   //! <b>Throws</b>: If pred throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() calls to the predicate.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class Pred>
   SizeType remove_if(Pred pred)
   {
      const NodePtr bbeg = this->get_root_node();
      typename NodeAlgorithms::stable_partition_info info;
      NodeAlgorithms::stable_partition
         (bbeg, this->get_end_node(), detail::KeyNodeptrComp<Pred, ValueTraits>(pred, &this->priv_value_traits()), info);
      //After cache last is set, slist invariants are preserved...
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         this->set_last_node(info.new_last_node);
      }
      //...so erase can be safely called
      this->erase_after( ConstIterator(bbeg, this->priv_value_traits_ptr())
                       , ConstIterator(info.beg_2st_partition, this->priv_value_traits_ptr())
                       , info.num_1st_partition);
      return info.num_1st_partition;
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes all the elements for which a specified
   //!   predicate is satisfied.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //!
   //! <b>Throws</b>: If pred throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() comparisons for equality.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class Pred, class Disposer>
   SizeType remove_and_dispose_if(Pred pred, Disposer disposer)
   {
      const NodePtr bbeg = this->get_root_node();
      typename NodeAlgorithms::stable_partition_info info;
      NodeAlgorithms::stable_partition
         (bbeg, this->get_end_node(), detail::KeyNodeptrComp<Pred, ValueTraits>(pred, &this->priv_value_traits()), info);
      //After cache last is set, slist invariants are preserved...
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         this->set_last_node(info.new_last_node);
      }
      //...so erase can be safely called
      this->erase_after_and_dispose( ConstIterator(bbeg, this->priv_value_traits_ptr())
                                   , ConstIterator(info.beg_2st_partition, this->priv_value_traits_ptr())
                                   , disposer);
      return info.num_1st_partition;
   }

   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that are equal from the list. No destructors are called.
   //!
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If operator== throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1) comparisons calls to pred()).
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   SizeType unique()
   {  return this->unique_and_dispose(ValueEqual<ValueType>(), detail::NullDisposer());  }

   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that satisfy some binary predicate from the list.
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If the predicate throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1) comparisons equality comparisons.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class BinaryPredicate>
   SizeType unique(BinaryPredicate pred)
   {  return this->unique_and_dispose(pred, detail::NullDisposer());  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that satisfy some binary predicate from the list.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //!
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If operator== throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1) comparisons equality comparisons.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class Disposer>
   SizeType unique_and_dispose(Disposer disposer)
   {  return this->unique_and_dispose(ValueEqual<ValueType>(), disposer);  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that satisfy some binary predicate from the list.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //!
   //! <b>Throws</b>: If the predicate throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1) comparisons equality comparisons.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class BinaryPredicate, class Disposer>
   SizeType unique_and_dispose(BinaryPredicate pred, Disposer disposer)
   {
      ConstIterator end_n(this->cend());
      ConstIterator bcur(this->cbegin());
      SizeType n = 0;

      if(bcur != end_n){
         ConstIterator cur(bcur);
         ++cur;
         while(cur != end_n) {
            if (pred(*bcur, *cur)){
               cur = this->erase_after_and_dispose(bcur, disposer);
               ++n;
            }
            else{
               bcur = cur;
               ++cur;
            }
         }
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            this->set_last_node(bcur.pointed_node());
         }
      }
      return n;
   }

   //! <b>Requires</b>: `value` must be a reference to a value inserted in an instance of this container type.
   //!
   //! <b>Effects</b>: This function returns a const_iterator pointing to the element
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   //!   This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static Iterator s_iterator_to(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      return Iterator (ValueTraits::to_node_ptr(value), ConstValueTraitsPtr());
   }

   //! <b>Requires</b>: value must be a const reference to a value inserted in a list.
   //!
   //! <b>Effects</b>: This function returns an iterator pointing to the element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   //!   This static function is available only if the <i>value traits</i>
   //!   is stateless.
   static ConstIterator s_iterator_to(ConstReference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((!STATEFUL_VALUE_TRAITS));
      Reference r =*detail::uncast(PointerTraits<ConstPointer>::pointer_to(value));
      return ConstIterator(ValueTraits::to_node_ptr(r), ConstValueTraitsPtr());
   }

   //! <b>Requires</b>: `value` must be a reference to a value inserted in an instance of this container type.
   //!
   //! <b>Effects</b>: This function returns a const_iterator pointing to the element
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   Iterator iterator_to(Reference value) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(LINEAR || !NodeAlgorithms::inited(this->priv_value_traits().to_node_ptr(value)));
      return Iterator (this->priv_value_traits().to_node_ptr(value), this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: value must be a const reference to a value inserted in a list.
   //!
   //! <b>Effects</b>: This function returns an iterator pointing to the element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   ConstIterator iterator_to(ConstReference value) const BOOST_NOEXCEPT
   {
      Reference r =*detail::uncast(PointerTraits<ConstPointer>::pointer_to(value));
      BOOST_INTRUSIVE_INVARIANT_ASSERT (LINEAR || !NodeAlgorithms::inited(this->priv_value_traits().to_node_ptr(r)));
      return ConstIterator(this->priv_value_traits().to_node_ptr(r), this->priv_value_traits_ptr());
   }

   //! <b>Returns</b>: The iterator to the element before i in the list.
   //!   Returns the end-iterator, if either i is the begin-iterator or the
   //!   list is empty.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements before i.
   //!   Constant if cache_last<> is true and i == end().
   Iterator previous(Iterator i) BOOST_NOEXCEPT
   {  return this->previous(this->cbefore_begin(), i); }

   //! <b>Returns</b>: The const_iterator to the element before i in the list.
   //!   Returns the end-const_iterator, if either i is the begin-const_iterator or
   //!   the list is empty.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements before i.
   //!   Constant if cache_last<> is true and i == end().
   ConstIterator previous(ConstIterator i) const BOOST_NOEXCEPT
   {  return this->previous(this->cbefore_begin(), i); }

   //! <b>Returns</b>: The iterator to the element before i in the list,
   //!   starting the search on element after prev_from.
   //!   Returns the end-iterator, if either i is the begin-iterator or the
   //!   list is empty.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements before i.
   //!   Constant if cache_last<> is true and i == end().
   Iterator previous(ConstIterator prev_from, Iterator i) BOOST_NOEXCEPT
   {  return this->previous(prev_from, ConstIterator(i)).unconst(); }

   //! <b>Returns</b>: The const_iterator to the element before i in the list,
   //!   starting the search on element after prev_from.
   //!   Returns the end-const_iterator, if either i is the begin-const_iterator or
   //!   the list is empty.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements before i.
   //!   Constant if cache_last<> is true and i == end().
   ConstIterator previous(ConstIterator prev_from, ConstIterator i) const BOOST_NOEXCEPT
   {
      if(CACHE_LAST && (i.pointed_node() == this->get_end_node())){
         return ConstIterator(detail::uncast(this->get_last_node()), this->priv_value_traits_ptr());
      }
      return ConstIterator
         (NodeAlgorithms::get_previous_node
            (prev_from.pointed_node(), i.pointed_node()), this->priv_value_traits_ptr());
   }

   ///@cond

   //! <b>Requires</b>: prev_pos must be a dereferenceable iterator in *this or be
   //!   before_begin(), and f and before_l belong to another slist.
   //!
   //! <b>Effects</b>: Transfers the range [f, before_l] to this
   //!   list, after the element pointed by prev_pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred
   //!   if constant_time_size is true. Constant-time otherwise.
   //!
   //! <b>Note</b>: Iterators of values obtained from the list that owned f and before_l now
   //!   point to elements of this list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Warning</b>: Experimental function, don't use it!
   void incorporate_after(ConstIterator prev_pos, NodePtr f, NodePtr before_l) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         this->incorporate_after(prev_pos, f, before_l, NodeAlgorithms::distance(f.pointed_node(), before_l.pointed_node())+1);
      else
         this->priv_incorporate_after(prev_pos.pointed_node(), f, before_l);
   }

   //! <b>Requires</b>: prev_pos must be a dereferenceable iterator in *this or be
   //!   before_begin(), and f and before_l belong to another slist.
   //!   n == distance(f, before_l) + 1.
   //!
   //! <b>Effects</b>: Transfers the range [f, before_l] to this
   //!   list, after the element pointed by prev_pos.
   //!   No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time.
   //!
   //! <b>Note</b>: Iterators of values obtained from the list that owned f and before_l now
   //!   point to elements of this list. Iterators of this list and all the references are not invalidated.
   //!
   //! <b>Warning</b>: Experimental function, don't use it!
   void incorporate_after(ConstIterator prev_pos, NodePtr f, NodePtr before_l, SizeType n) BOOST_NOEXCEPT
   {
      if(n){
         BOOST_INTRUSIVE_INVARIANT_ASSERT(n > 0);
         BOOST_INTRUSIVE_INVARIANT_ASSERT
            (SizeType(boost::intrusive::iterator_distance
               ( Iterator(f, this->priv_value_traits_ptr())
               , Iterator(before_l, this->priv_value_traits_ptr())))
            +1 == n);
         this->priv_incorporate_after(prev_pos.pointed_node(), f, before_l);
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
            this->priv_size_traits().increase(n);
         }
      }
   }

   ///@endcond

   //! <b>Effects</b>: Asserts the integrity of the container.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Note</b>: The method has no effect when asserts are turned off (e.g., with NDEBUG).
   //!   Experimental function, interface might change in future versions.
   void check() const
   {
      ConstNodePtr header_ptr = get_root_node();
      // header's next is never null
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_next(header_ptr));
      if (NodeTraits::get_next(header_ptr) == header_ptr)
      {
         BOOST_INTRUSIVE_INVARIANT_ASSERT(!CONSTANT_TIME_SIZE || this->priv_size_traits().get_size() == 0);
         return;
      }
      size_t node_count = 0; (void)node_count;
      ConstNodePtr p = header_ptr;
      while (true)
      {
         ConstNodePtr next_p = NodeTraits::get_next(p);
         BOOST_IF_CONSTEXPR(!LINEAR)
         {
            BOOST_INTRUSIVE_INVARIANT_ASSERT(next_p);
         }
         else
         {
            BOOST_INTRUSIVE_INVARIANT_ASSERT(next_p != header_ptr);
         }
         if ((!LINEAR && next_p == header_ptr) || (LINEAR && !next_p))
         {
            BOOST_INTRUSIVE_INVARIANT_ASSERT(!CACHE_LAST || get_last_node() == p);
            break;
         }
         p = next_p;
         ++node_count;
      }
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!CONSTANT_TIME_SIZE || this->priv_size_traits().get_size() == node_count);
   }


   friend bool operator==(const SlistImpl &x, const SlistImpl &y)
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
      if(x.size() != y.size()){
         return false;
      }
      return ::boost::intrusive::algo_equal(x.cbegin(), x.cend(), y.cbegin(), y.cend());
   }

   friend bool operator!=(const SlistImpl &x, const SlistImpl &y)
   {  return !(x == y); }

   friend bool operator<(const SlistImpl &x, const SlistImpl &y)
   {  return ::boost::intrusive::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   friend bool operator>(const SlistImpl &x, const SlistImpl &y)
   {  return y < x;  }

   friend bool operator<=(const SlistImpl &x, const SlistImpl &y)
   {  return !(y < x);  }

   friend bool operator>=(const SlistImpl &x, const SlistImpl &y)
   {  return !(x < y);  }

   friend void swap(SlistImpl &x, SlistImpl &y)
   {  x.swap(y);  }

   private:
   void priv_splice_after(NodePtr prev_pos_n, SlistImpl &x, NodePtr before_f_n, NodePtr before_l_n)
   {
      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(before_f_n != before_l_n){
         if(prev_pos_n == this->get_last_node()){
            this->set_last_node(before_l_n);
         }
         if(&x != this && NodeTraits::get_next(before_l_n) == x.get_end_node()){
            x.set_last_node(before_f_n);
         }
      }
      NodeAlgorithms::transfer_after(prev_pos_n, before_f_n, before_l_n);
   }

   void priv_incorporate_after(NodePtr prev_pos_n, NodePtr first_n, NodePtr before_l_n)
   {
      BOOST_IF_CONSTEXPR(CACHE_LAST){
         if(prev_pos_n == this->get_last_node()){
            this->set_last_node(before_l_n);
         }
      }
      NodeAlgorithms::incorporate_after(prev_pos_n, first_n, before_l_n);
   }

   void priv_reverse(detail::bool_<false>)
   {  NodeAlgorithms::reverse(this->get_root_node());   }

   void priv_reverse(detail::bool_<true>)
   {
      NodePtr new_first = NodeAlgorithms::reverse
         (NodeTraits::get_next(this->get_root_node()));
      NodeTraits::set_next(this->get_root_node(), new_first);
   }

   void priv_shift_backwards(SizeType n, detail::bool_<false>)
   {
      NodePtr l = NodeAlgorithms::move_forward(this->get_root_node(), (std::size_t)n);
      (void)l;

      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(l){
         this->set_last_node(l);
      }
   }

   void priv_shift_backwards(SizeType n, detail::bool_<true>)
   {
      typename NodeAlgorithms::node_pair ret(
         NodeAlgorithms::move_first_n_forward
            (NodeTraits::get_next(this->get_root_node()), (std::size_t)n));
      if(ret.first){
         NodeTraits::set_next(this->get_root_node(), ret.first);
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            this->set_last_node(ret.second);
         }
      }
   }

   void priv_shift_forward(SizeType n, detail::bool_<false>)
   {
      NodePtr l = NodeAlgorithms::move_backwards(this->get_root_node(), (std::size_t)n);
      (void)l;

      BOOST_IF_CONSTEXPR(CACHE_LAST)
      if(l){
         this->set_last_node(l);
      }
   }

   void priv_shift_forward(SizeType n, detail::bool_<true>)
   {
      typename NodeAlgorithms::node_pair ret(
         NodeAlgorithms::move_first_n_backwards
         (NodeTraits::get_next(this->get_root_node()), (std::size_t)n));
      if(ret.first){
         NodeTraits::set_next(this->get_root_node(), ret.first);
         BOOST_IF_CONSTEXPR(CACHE_LAST){
            this->set_last_node(ret.second);
         }
      }
   }

   static void priv_swap_cache_last(SlistImpl *this_impl, SlistImpl *other_impl)
   {
      bool other_was_empty = false;
      if(this_impl->empty()){
         //Check if both are empty or
         if(other_impl->empty())
            return;
         //If this is empty swap pointers
         SlistImpl *tmp = this_impl;
         this_impl  = other_impl;
         other_impl = tmp;
         other_was_empty = true;
      }
      else{
         other_was_empty = other_impl->empty();
      }

      //Precondition: this is not empty
      NodePtr other_old_last(other_impl->get_last_node());
      NodePtr other_bfirst(other_impl->get_root_node());
      NodePtr this_bfirst(this_impl->get_root_node());
      NodePtr this_old_last(this_impl->get_last_node());

      //Move all nodes from this to other's beginning
      NodeAlgorithms::transfer_after(other_bfirst, this_bfirst, this_old_last);
      other_impl->set_last_node(this_old_last);

      if(other_was_empty){
         this_impl->set_last_node(this_bfirst);
      }
      else{
         //Move trailing nodes from other to this
         NodeAlgorithms::transfer_after(this_bfirst, this_old_last, other_old_last);
         this_impl->set_last_node(other_old_last);
      }
   }

   //circular version
   static void priv_swap_lists(NodePtr this_node, NodePtr other_node, detail::bool_<false>)
   {  NodeAlgorithms::swap_nodes(this_node, other_node); }

   //linear version
   static void priv_swap_lists(NodePtr this_node, NodePtr other_node, detail::bool_<true>)
   {  NodeAlgorithms::swap_trailing_nodes(this_node, other_node); }

   BOOST_INTRUSIVE_NO_DANGLING
   static SlistImpl &priv_container_from_end_iterator(const ConstIterator &end_iterator)
   {
      //Obtaining the container from the end iterator is not possible with linear
      //singly linked lists (because "end" is represented by the null pointer)
      BOOST_INTRUSIVE_STATIC_ASSERT(!LINEAR);
      BOOST_INTRUSIVE_STATIC_ASSERT((HAS_CONTAINER_FROM_ITERATOR));
      NodePtr p = end_iterator.pointed_node();
      HeaderHolderType* h = HeaderHolderType::get_holder(p);
      HeaderHolderPlusLastT* hpl = detail::parent_from_member< HeaderHolderPlusLastT, HeaderHolderType>
                                         (h, &HeaderHolderPlusLastT::header_holder_);
      RootPlusSize* r = static_cast< RootPlusSize* >(hpl);
      DataT *d = detail::parent_from_member<DataT, RootPlusSize>
         ( r, &DataT::root_plus_size_);
      SlistImpl *s  = detail::parent_from_member<SlistImpl, DataT>(d, &SlistImpl::m_data);
      return *s;
   }
};

//! Helper metafunction to define a \c slist that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void, class O3 = void, class O4 = void, class O5 = void, class O6 = void>
#endif
struct MakeSlist
{
   /// @cond
   typedef typename PackOptions
      < SlistDefaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6
         #else
         Options...
         #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;
   typedef SlistImpl
      < ValueTraits
      , typename PackedOptions::size_type
      ,  (std::size_t(PackedOptions::linear)*SlistBoolFlags::LINEAR_POS)
        |(std::size_t(PackedOptions::constant_time_size)*SlistBoolFlags::CONSTANT_TIME_SIZE_POS)
        |(std::size_t(PackedOptions::cache_last)*SlistBoolFlags::CACHE_LAST_POS)
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
class Slist
   :  public MakeSlist<T,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4, O5, O6
         #else
         Options...
         #endif
      >::type
{
   typedef typename MakeSlist
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;
   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Slist)

   public:
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::SizeType          SizeType;
   typedef typename Base::NodePtr           NodePtr;

   inline Slist()
      :  Base()
   {}

   inline explicit Slist(const ValueTraits &v_traits)
      :  Base(v_traits)
   {}

   struct IncorporateT{};

   inline Slist( NodePtr f, NodePtr before_l
             , SizeType n, const ValueTraits &v_traits = ValueTraits())
      :  Base(f, before_l, n, v_traits)
   {}

   template<class Iterator>
   inline Slist(Iterator b, Iterator e, const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, v_traits)
   {}

   inline Slist(BOOST_RV_REF(Slist) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Slist& operator=(BOOST_RV_REF(Slist) x)
   {  return static_cast<Slist &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Slist &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Slist) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Slist &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Slist &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Slist &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Slist &>(Base::container_from_end_iterator(end_iterator));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SLIST_HPP
