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

#ifndef BOOST_INTRUSIVE_LIST_HPP
#define BOOST_INTRUSIVE_LIST_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/intrusive/circular_list_algorithms.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/detail/is_stateful_value_traits.hpp>
#include <boost/intrusive/detail/default_header_holder.hpp>
#include <boost/intrusive/detail/reverse_iterator.hpp>
#include <boost/intrusive/detail/uncast.hpp>
#include <boost/intrusive/detail/list_iterator.hpp>
#include <boost/intrusive/detail/array_initializer.hpp>
#include <boost/intrusive/detail/exception_disposer.hpp>
#include <boost/intrusive/detail/equal_to_value.hpp>
#include <boost/intrusive/detail/key_nodeptr_comp.hpp>
#include <boost/intrusive/detail/simple_disposers.hpp>
#include <boost/intrusive/detail/size_holder.hpp>
#include <boost/intrusive/detail/algorithm.hpp>

#include <boost/move/utility_core.hpp>

#include <boost/intrusive/detail/value_functors.hpp>
#include <cstddef>   //std::size_t, etc.

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

struct DefaultListHookApplier
{  template <class T> struct Apply{ typedef typename T::default_list_hook Type;  };  };

template<>
struct IsDefaultHookTag<DefaultListHookApplier>
{  static const bool VALUE = true;  };

struct ListDefaults
{
   typedef DefaultListHookApplier ProtoValueTraits;
   static const bool CONSTANT_TIME_SIZE = true;
   typedef std::size_t SizeType;
   typedef void HeaderHolderType;
};

/// @endcond

//! The class template list is an intrusive container that mimics most of the
//! interface of std::list as described in the C++ standard.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c constant_time_size<> and \c size_type<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template <class TValueTraits, class TSizeType, bool ConstantTimeSize, typename HeaderHolder>
#endif
class ListImpl
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
   typedef ListIterator<ValueTraits, false>                        Iterator;
   typedef ListIterator<ValueTraits, true>                         ConstIterator;
   typedef boost::intrusive::ReverseIterator<Iterator>              ReverseIterator;
   typedef boost::intrusive::ReverseIterator<ConstIterator>        ConstReverseIterator;
   typedef typename ValueTraits::NodeTraits                        NodeTraits;
   typedef typename NodeTraits::Node                                Node;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef CircularListAlgorithms<NodeTraits>                     NodeAlgorithms;
   typedef typename detail::GetHeaderHolderType
      < ValueTraits, HeaderHolder >::type                           HeaderHolderType;

   static const bool CONSTANT_TIME_SIZE = ConstantTimeSize;
   static const bool STATEFUL_VALUE_TRAITS = detail::IsStatefulValueTraits<ValueTraits>::value;
   static const bool HAS_CONTAINER_FROM_ITERATOR =
        detail::is_same< HeaderHolderType, detail::DefaultHeaderHolder< NodeTraits > >::value;

   /// @cond

   private:
   typedef detail::SizeHolder<CONSTANT_TIME_SIZE, SizeType>          SizeTraits;

   //noncopyable
   BOOST_MOVABLE_BUT_NOT_COPYABLE(ListImpl)

   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<ValueTraits::link_mode>::VALUE;

   //Constant-time size is incompatible with auto-unlink hooks!
   BOOST_INTRUSIVE_STATIC_ASSERT(!(CONSTANT_TIME_SIZE &&
                        ((int)ValueTraits::link_mode == (int)AutoUnlink)
                      ));

   inline NodePtr get_root_node()
   { return m_data.m_root_plus_size.m_header.get_node(); }

   inline ConstNodePtr get_root_node() const
   { return m_data.m_root_plus_size.m_header.get_node(); }

   struct RootPlusSize : public SizeTraits
   {
      HeaderHolderType m_header;
   };

   struct DataT : public ValueTraits
   {
      typedef typename ListImpl::ValueTraits ValueTraits;
      inline explicit DataT(const ValueTraits &val_traits)
         :  ValueTraits(val_traits)
      {}

      RootPlusSize m_root_plus_size;
   } m_data;

   inline SizeTraits &priv_size_traits() BOOST_NOEXCEPT
   {  return m_data.m_root_plus_size;  }

   inline const SizeTraits &priv_size_traits() const BOOST_NOEXCEPT
   {  return m_data.m_root_plus_size;  }

   inline const ValueTraits &priv_value_traits() const BOOST_NOEXCEPT
   {  return m_data;  }

   inline ValueTraits &priv_value_traits() BOOST_NOEXCEPT
   {  return m_data;  }

   typedef typename boost::intrusive::ValueTraitsPointers
      <TValueTraits>::ConstValueTraitsPtr ConstValueTraitsPtr;

   inline ConstValueTraitsPtr priv_value_traits_ptr() const BOOST_NOEXCEPT
   {  return PointerTraits<ConstValueTraitsPtr>::pointer_to(this->priv_value_traits());  }

   /// @endcond

   public:

   //! <b>Effects</b>: constructs an empty list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   ListImpl()
      :  m_data(ValueTraits())
   {
      this->priv_size_traits().set_size(SizeType(0));
      NodeAlgorithms::init_header(this->get_root_node());
   }

   //! <b>Effects</b>: constructs an empty list.
   //!
   //! <b>Complexity</b>: Constant
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   explicit ListImpl(const ValueTraits &v_traits)
      :  m_data(v_traits)
   {
      this->priv_size_traits().set_size(SizeType(0));
      NodeAlgorithms::init_header(this->get_root_node());
   }

   //! <b>Requires</b>: Dereferencing iterator must yield an lvalue of type value_type.
   //!
   //! <b>Effects</b>: Constructs a list equal to the range [first,last).
   //!
   //! <b>Complexity</b>: Linear in distance(b, e). No copy constructors are called.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks).
   template<class Iterator>
   ListImpl(Iterator b, Iterator e, const ValueTraits &v_traits = ValueTraits())
      :  m_data(v_traits)
   {
      //nothrow, no need to rollback to release elements on exception
      this->priv_size_traits().set_size(SizeType(0));
      NodeAlgorithms::init_header(this->get_root_node());
      //nothrow, no need to rollback to release elements on exception
      this->insert(this->cend(), b, e);
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
   ListImpl(BOOST_RV_REF(ListImpl) x)
      : m_data(::boost::move(x.priv_value_traits()))
   {
      this->priv_size_traits().set_size(SizeType(0));
      NodeAlgorithms::init_header(this->get_root_node());
      //nothrow, no need to rollback to release elements on exception
      this->swap(x);
   }

   //! <b>Effects</b>: Equivalent to swap
   //!
   ListImpl& operator=(BOOST_RV_REF(ListImpl) x)
   {  this->swap(x); return *this;  }

   //! <b>Effects</b>: If it's not a safe-mode or an auto-unlink value_type
   //!   the destructor does nothing
   //!   (ie. no code is generated). Otherwise it detaches all elements from this.
   //!   In this case the objects in the list are not deleted (i.e. no destructors
   //!   are called), but the hooks according to the ValueTraits template parameter
   //!   are set to their default value.
   //!
   //! <b>Throws</b>: Nothing.
   //! 
   //! <b>Complexity</b>: Linear to the number of elements in the list, if
   //!   it's a safe-mode or auto-unlink value . Otherwise constant.
   ~ListImpl()
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
   ~ListImpl() requires (TValueTraits::link_mode == NormalLink) = default;
   #endif

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
   void push_back(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert = priv_value_traits().to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(to_insert));
      NodeAlgorithms::link_before(this->get_root_node(), to_insert);
      this->priv_size_traits().increment();
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
      NodeAlgorithms::link_before(NodeTraits::get_next(this->get_root_node()), to_insert);
      this->priv_size_traits().increment();
   }

   //! <b>Effects</b>: Erases the last element of the list.
   //!   No destructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the erased element.
   void pop_back() BOOST_NOEXCEPT
   {  return this->pop_back_and_dispose(detail::NullDisposer());   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the last element of the list.
   //!   No destructors are called.
   //!   Disposer::operator()(pointer) is called for the removed element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased element.
   template<class Disposer>
   void pop_back_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr to_erase = NodeTraits::get_previous(this->get_root_node());
      NodeAlgorithms::unlink(to_erase);
      this->priv_size_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(priv_value_traits().to_value_ptr(to_erase));
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
   //!   No destructors are called.
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
      NodeAlgorithms::unlink(to_erase);
      this->priv_size_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(priv_value_traits().to_value_ptr(to_erase));
   }

   //! <b>Effects</b>: Returns a reference to the first element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Reference front() BOOST_NOEXCEPT
   { return *priv_value_traits().to_value_ptr(NodeTraits::get_next(this->get_root_node())); }

   //! <b>Effects</b>: Returns a const_reference to the first element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReference front() const BOOST_NOEXCEPT
   { return *priv_value_traits().to_value_ptr(NodeTraits::get_next(this->get_root_node())); }

   //! <b>Effects</b>: Returns a reference to the last element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Reference back() BOOST_NOEXCEPT
   { return *priv_value_traits().to_value_ptr(NodeTraits::get_previous(this->get_root_node())); }

   //! <b>Effects</b>: Returns a const_reference to the last element of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReference back() const BOOST_NOEXCEPT
   { return *priv_value_traits().to_value_ptr(detail::uncast(NodeTraits::get_previous(this->get_root_node()))); }

   //! <b>Effects</b>: Returns an iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline Iterator begin() BOOST_NOEXCEPT
   { return Iterator(NodeTraits::get_next(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the first element contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator begin() const BOOST_NOEXCEPT
   { return this->cbegin(); }

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
   { return Iterator(this->get_root_node(), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a const_iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator end() const BOOST_NOEXCEPT
   { return this->cend(); }

   //! <b>Effects</b>: Returns a constant iterator to the end of the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstIterator cend() const BOOST_NOEXCEPT
   { return ConstIterator(detail::uncast(this->get_root_node()), this->priv_value_traits_ptr()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ReverseIterator rbegin() BOOST_NOEXCEPT
   { return ReverseIterator(this->end()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReverseIterator rbegin() const BOOST_NOEXCEPT
   { return this->crbegin(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the beginning
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReverseIterator crbegin() const BOOST_NOEXCEPT
   { return ConstReverseIterator(end()); }

   //! <b>Effects</b>: Returns a reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ReverseIterator rend() BOOST_NOEXCEPT
   { return ReverseIterator(begin()); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReverseIterator rend() const BOOST_NOEXCEPT
   { return this->crend(); }

   //! <b>Effects</b>: Returns a const_reverse_iterator pointing to the end
   //! of the reversed list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   inline ConstReverseIterator crend() const BOOST_NOEXCEPT
   { return ConstReverseIterator(this->begin()); }

   //! <b>Precondition</b>: end_iterator must be a valid end iterator
   //!   of list.
   //!
   //! <b>Effects</b>: Returns a const reference to the list associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   inline static ListImpl &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return ListImpl::priv_container_from_end_iterator(end_iterator);   }

   //! <b>Precondition</b>: end_iterator must be a valid end const_iterator
   //!   of list.
   //!
   //! <b>Effects</b>: Returns a const reference to the list associated to the end iterator
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   BOOST_INTRUSIVE_NO_DANGLING
   inline static const ListImpl &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return ListImpl::priv_container_from_end_iterator(end_iterator);   }

   //! <b>Effects</b>: Returns the number of the elements contained in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements contained in the list.
   //!   if constant-time size option is disabled. Constant time otherwise.
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
   {  return NodeAlgorithms::unique(this->get_root_node());   }

   //! <b>Effects</b>: Swaps the elements of x and *this.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   void swap(ListImpl& other) BOOST_NOEXCEPT
   {
      NodeAlgorithms::swap_nodes(this->get_root_node(), other.get_root_node());
      this->priv_size_traits().swap(other.priv_size_traits());
   }

   //! <b>Effects</b>: Moves backwards all the elements, so that the first
   //!   element becomes the second, the second becomes the third...
   //!   the last element becomes the first one.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of shifts.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   inline void shift_backwards(SizeType n = 1) BOOST_NOEXCEPT
   {  NodeAlgorithms::move_forward(this->get_root_node(), n);  }

   //! <b>Effects</b>: Moves forward all the elements, so that the second
   //!   element becomes the first, the third becomes the second...
   //!   the first element becomes the last one.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of shifts.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   inline void shift_forward(SizeType n = 1) BOOST_NOEXCEPT
   {  NodeAlgorithms::move_backwards(this->get_root_node(), n);  }

   //! <b>Effects</b>: Erases the element pointed by i of the list.
   //!   No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed element,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased element.
   inline Iterator erase(ConstIterator i) BOOST_NOEXCEPT
   {  return this->erase_and_dispose(i, detail::NullDisposer());  }

   //! <b>Requires</b>: b and e must be valid iterators to elements in *this.
   //!
   //! <b>Effects</b>: Erases the element range pointed by b and e
   //! No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of erased elements if it's a safe-mode
   //!   or auto-unlink value, or constant-time size is enabled. Constant-time otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased elements.
   inline Iterator erase(ConstIterator b, ConstIterator e) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK || CONSTANT_TIME_SIZE){
         return this->erase_and_dispose(b, e, detail::NullDisposer());
      }
      else{
         NodeAlgorithms::unlink(b.pointed_node(), e.pointed_node());
         return e.unconst();
      }
   }

   //! <b>Requires</b>: b and e must be valid iterators to elements in *this.
   //!   n must be distance(b, e).
   //!
   //! <b>Effects</b>: Erases the element range pointed by b and e
   //! No destructors are called.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of erased elements if it's a safe-mode
   //!   or auto-unlink value is enabled. Constant-time otherwise.
   //!
   //! <b>Note</b>: Invalidates the iterators (but not the references) to the
   //!   erased elements.
   Iterator erase(ConstIterator b, ConstIterator e, SizeType n) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeAlgorithms::distance(b.pointed_node(), e.pointed_node()) == n);
      (void)n;
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK){
         return this->erase_and_dispose(b, e, detail::NullDisposer());
      }
      else{
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
            this->priv_size_traits().decrease(n);
         }
         NodeAlgorithms::unlink(b.pointed_node(), e.pointed_node());
         return e.unconst();
      }
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
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased element.
   template <class Disposer>
   Iterator erase_and_dispose(ConstIterator i, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr to_erase(i.pointed_node());
      ++i;
      NodeAlgorithms::unlink(to_erase);
      this->priv_size_traits().decrement();
      BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
         NodeAlgorithms::init(to_erase);
      disposer(this->priv_value_traits().to_value_ptr(to_erase));
      return i.unconst();
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   template<class Disposer>
   Iterator erase_and_dispose(Iterator i, Disposer disposer) BOOST_NOEXCEPT
   {  return this->erase_and_dispose(ConstIterator(i), disposer);   }
   #endif

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases the element range pointed by b and e
   //!   No destructors are called.
   //!   Disposer::operator()(pointer) is called for the removed elements.
   //!
   //! <b>Returns</b>: the first element remaining beyond the removed elements,
   //!   or end() if no such element exists.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements erased.
   //!
   //! <b>Note</b>: Invalidates the iterators to the erased elements.
   template <class Disposer>
   Iterator erase_and_dispose(ConstIterator b, ConstIterator e, Disposer disposer) BOOST_NOEXCEPT
   {
      NodePtr bp(b.pointed_node()), ep(e.pointed_node());
      NodeAlgorithms::unlink(bp, ep);
      while(bp != ep){
         NodePtr to_erase(bp);
         bp = NodeTraits::get_next(bp);
         BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
            NodeAlgorithms::init(to_erase);
         disposer(priv_value_traits().to_value_ptr(to_erase));
         this->priv_size_traits().decrement();
      }
      return e.unconst();
   }

   //! <b>Effects</b>: Erases all the elements of the container.
   //!   No destructors are called.
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
         NodeAlgorithms::init_header(this->get_root_node());
         this->priv_size_traits().set_size(SizeType(0));
      }
   }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Erases all the elements of the container.
   //!   No destructors are called.
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
      ConstIterator it(this->begin()), itend(this->end());
      while(it != itend){
         NodePtr to_erase(it.pointed_node());
         ++it;
         BOOST_IF_CONSTEXPR(SAFEMODE_OR_AUTOUNLINK)
            NodeAlgorithms::init(to_erase);
         disposer(priv_value_traits().to_value_ptr(to_erase));
      }
      NodeAlgorithms::init_header(this->get_root_node());
      this->priv_size_traits().set_size(0);
   }

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
   //! <b>Throws</b>: If cloner throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(const ListImpl &src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      detail::ExceptionDisposer<ListImpl, Disposer>
         rollback(*this, disposer);
      ConstIterator b(src.begin()), e(src.end());
      for(; b != e; ++b){
         this->push_back(*cloner(*b));
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
   //! <b>Throws</b>: If cloner throws. Basic guarantee.
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(ListImpl) src, Cloner cloner, Disposer disposer)
   {
      this->clear_and_dispose(disposer);
      detail::ExceptionDisposer<ListImpl, Disposer>
         rollback(*this, disposer);
      Iterator b(src.begin()), e(src.end());
      for(; b != e; ++b){
         this->push_back(*cloner(*b));
      }
      rollback.release();
   }

   //! <b>Requires</b>: value must be an lvalue and p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Inserts the value before the position pointed by p.
   //!
   //! <b>Returns</b>: An iterator to the inserted element.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant time. No copy constructors are called.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   Iterator insert(ConstIterator p, Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert = this->priv_value_traits().to_node_ptr(value);
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SAFEMODE_OR_AUTOUNLINK || NodeAlgorithms::inited(to_insert));
      NodeAlgorithms::link_before(p.pointed_node(), to_insert);
      this->priv_size_traits().increment();
      return Iterator(to_insert, this->priv_value_traits_ptr());
   }

   //! <b>Requires</b>: Dereferencing iterator must yield
   //!   an lvalue of type value_type and p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Inserts the range pointed by b and e before the position p.
   //!   No copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements inserted.
   //!
   //! <b>Note</b>: Does not affect the validity of iterators and references.
   template<class Iterator>
   void insert(ConstIterator p, Iterator b, Iterator e) BOOST_NOEXCEPT
   {
      for (; b != e; ++b)
         this->insert(p, *b);
   }

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
   void assign(Iterator b, Iterator e) BOOST_NOEXCEPT
   {
      this->clear();
      this->insert(this->cend(), b, e);
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
   void dispose_and_assign(Disposer disposer, Iterator b, Iterator e) BOOST_NOEXCEPT
   {
      this->clear_and_dispose(disposer);
      this->insert(this->cend(), b, e);
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!
   //! <b>Effects</b>: Transfers all the elements of list x to this list, before the
   //!   the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of
   //!    this list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator p, ListImpl& x) BOOST_NOEXCEPT
   {
      if(!x.empty()){
         NodeAlgorithms::transfer
            (p.pointed_node(), x.begin().pointed_node(), x.end().pointed_node());
         SizeTraits &thist = this->priv_size_traits();
         SizeTraits &xt = x.priv_size_traits();
         thist.increase(xt.get_size());
         xt.set_size(SizeType(0));
      }
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!   new_ele must point to an element contained in list x.
   //!
   //! <b>Effects</b>: Transfers the value pointed by new_ele, from list x to this list,
   //!   before the element pointed by p. No destructors or copy constructors are called.
   //!   If p == new_ele or p == ++new_ele, this function is a null operation.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator p, ListImpl&x, ConstIterator new_ele) BOOST_NOEXCEPT
   {
      NodeAlgorithms::transfer(p.pointed_node(), new_ele.pointed_node());
      x.priv_size_traits().decrement();
      this->priv_size_traits().increment();
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!   f and e must point to elements contained in list x.
   //!
   //! <b>Effects</b>: Transfers the range pointed by f and e from list x to this list,
   //!   before the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the number of elements transferred
   //!   if constant-time size option is enabled. Constant-time otherwise.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator p, ListImpl&x, ConstIterator f, ConstIterator e) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         this->splice(p, x, f, e, NodeAlgorithms::distance(f.pointed_node(), e.pointed_node()));
      else
         this->splice(p, x, f, e, 1);//intrusive::iterator_distance is a dummy value
   }

   //! <b>Requires</b>: p must be a valid iterator of *this.
   //!   f and e must point to elements contained in list x.
   //!   n == distance(f, e)
   //!
   //! <b>Effects</b>: Transfers the range pointed by f and e from list x to this list,
   //!   before the element pointed by p. No destructors or copy constructors are called.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   //!
   //! <b>Note</b>: Iterators of values obtained from list x now point to elements of this
   //!   list. Iterators of this list and all the references are not invalidated.
   void splice(ConstIterator p, ListImpl&x, ConstIterator f, ConstIterator e, SizeType n) BOOST_NOEXCEPT
   {
      if(n){
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE){
            BOOST_INTRUSIVE_INVARIANT_ASSERT(n == NodeAlgorithms::distance(f.pointed_node(), e.pointed_node()));
            NodeAlgorithms::transfer(p.pointed_node(), f.pointed_node(), e.pointed_node());
            SizeTraits &thist = this->priv_size_traits();
            SizeTraits &xt = x.priv_size_traits();
            thist.increase(n);
            xt.decrease(n);
         }
         else{
            NodeAlgorithms::transfer(p.pointed_node(), f.pointed_node(), e.pointed_node());
         }
      }
   }

   //! <b>Effects</b>: This function sorts the list *this according to operator <.
   //!   The sort is stable, that is, the relative order of equivalent elements is preserved.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or operator < throws. Basic guarantee.
   //!
   //! <b>Notes</b>: Iterators and references are not invalidated.
   //!
   //! <b>Complexity</b>: The number of comparisons is approximately N log N, where N
   //!   is the list's size.
   void sort()
   {  this->sort(ValueLess<ValueType>());  }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak ordering
   //!
   //! <b>Effects</b>: This function sorts the list *this according to p. The sort is
   //!   stable, that is, the relative order of equivalent elements is preserved.
   //!
   //! <b>Throws</b>: If value_traits::node_traits::node
   //!   constructor throws (this does not happen with predefined Boost.Intrusive hooks)
   //!   or the predicate throws. Basic guarantee.
   //!
   //! <b>Notes</b>: This won't throw if list_base_hook<> or
   //!   list_member_hook are used.
   //!   Iterators and references are not invalidated.
   //!
   //! <b>Complexity</b>: The number of comparisons is approximately N log N, where N
   //!   is the list's size.
   template<class Predicate>
   void sort(Predicate p)
   {
      if(NodeTraits::get_next(this->get_root_node())
         != NodeTraits::get_previous(this->get_root_node())){
         ListImpl carry(this->priv_value_traits());
         detail::ArrayInitializer<ListImpl, 64> counter(this->priv_value_traits());
         int fill = 0;
         while(!this->empty()){
            carry.splice(carry.cbegin(), *this, this->cbegin());
            int i = 0;
            while(i < fill && !counter[i].empty()) {
               counter[i].merge(carry, p);
               carry.swap(counter[i++]);
            }
            carry.swap(counter[i]);
            if(i == fill)
               ++fill;
         }
         for (int i = 1; i < fill; ++i)
            counter[i].merge(counter[i-1], p);
         this->swap(counter[fill-1]);
      }
   }

   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this according to operator <. The merge is stable;
   //!   that is, if an element from *this is equivalent to one from x, then the element
   //!   from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If operator < throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   void merge(ListImpl& x)
   { this->merge(x, ValueLess<ValueType>()); }

   //! <b>Requires</b>: p must be a comparison function that induces a strict weak
   //!   ordering and both *this and x must be sorted according to that ordering
   //!   The lists x and *this must be distinct.
   //!
   //! <b>Effects</b>: This function removes all of x's elements and inserts them
   //!   in order into *this. The merge is stable; that is, if an element from *this is
   //!   equivalent to one from x, then the element from *this will precede the one from x.
   //!
   //! <b>Throws</b>: If the predicate throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: This function is linear time: it performs at most
   //!   size() + x.size() - 1 comparisons.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated.
   template<class Predicate>
   void merge(ListImpl& x, Predicate p)
   {
      ConstIterator e(this->cend()), ex(x.cend());
      ConstIterator b(this->cbegin());
      while(!x.empty()){
         ConstIterator ix(x.cbegin());
         while (b != e && !p(*ix, *b)){
            ++b;
         }
         if(b == e){
            //Now transfer the rest to the end of the container
            this->splice(e, x);
            break;
         }
         else{
            SizeType n(0);
            do{
               ++ix; ++n;
            } while(ix != ex && p(*ix, *b));
            this->splice(b, x, x.begin(), ix, n);
         }
      }
   }

   //! <b>Effects</b>: Reverses the order of elements in the list.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: This function is linear time.
   //!
   //! <b>Note</b>: Iterators and references are not invalidated
   void reverse() BOOST_NOEXCEPT
   {  NodeAlgorithms::reverse(this->get_root_node());   }

   //! <b>Effects</b>: Removes all the elements that compare equal to value.
   //!   No destructors are called.
   //!
   //! <b>Throws</b>: If operator == throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time. It performs exactly size() comparisons for equality.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   SizeType remove(ConstReference value) BOOST_NOEXCEPT
   {  return this->remove_if(detail::EqualToValue<ConstReference>(value));  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes all the elements that compare equal to value.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //! 
   //! <b>Returns</b>: The number of erased elements.
   //!
   //! <b>Throws</b>: If operator == throws. Basic guarantee.
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
   //! <b>Returns</b>: The number of removed elements.
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
      const NodePtr root_node = this->get_root_node();
      typename NodeAlgorithms::stable_partition_info info;
      NodeAlgorithms::stable_partition
         (NodeTraits::get_next(root_node), root_node, detail::KeyNodeptrComp<Pred, ValueTraits>(pred, &this->priv_value_traits()), info);
      //Invariants preserved by stable_partition so erase can be safely called
      //The first element might have changed so calculate it again
      this->erase( ConstIterator(NodeTraits::get_next(root_node), this->priv_value_traits_ptr())
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
   //! <b>Returns</b>: The number of removed elements.
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
      const NodePtr root_node = this->get_root_node();
      typename NodeAlgorithms::stable_partition_info info;
      NodeAlgorithms::stable_partition
         (NodeTraits::get_next(root_node), root_node, detail::KeyNodeptrComp<Pred, ValueTraits>(pred, &this->priv_value_traits()), info);
      //Invariants preserved by stable_partition so erase can be safely called
      //The first element might have changed so calculate it again
      this->erase_and_dispose( ConstIterator(NodeTraits::get_next(root_node), this->priv_value_traits_ptr())
                             , ConstIterator(info.beg_2st_partition, this->priv_value_traits_ptr())
                             , disposer);
      return info.num_1st_partition;
   }

   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that are equal from the list. No destructors are called.
   //!
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If the comparison operator throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1 comparisons calls to pred()).
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
   //! <b>Throws</b>: If pred throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1 comparisons equality comparisons).
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class BinaryPredicate>
   SizeType unique(BinaryPredicate pred)
   {  return this->unique_and_dispose(pred, detail::NullDisposer());  }

   //! <b>Requires</b>: Disposer::operator()(pointer) shouldn't throw.
   //!
   //! <b>Effects</b>: Removes adjacent duplicate elements or adjacent
   //!   elements that are equal from the list.
   //!   Disposer::operator()(pointer) is called for every removed element.
   //!
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If the equality operator throws. Basic guarantee.
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
   //! <b>Returns</b>: The number of removed elements.
   //!
   //! <b>Throws</b>: If pred throws. Basic guarantee.
   //!
   //! <b>Complexity</b>: Linear time (size()-1) comparisons equality comparisons.
   //!
   //! <b>Note</b>: The relative order of elements that are not removed is unchanged,
   //!   and iterators to elements that are not removed remain valid.
   template<class BinaryPredicate, class Disposer>
   SizeType unique_and_dispose(BinaryPredicate pred, Disposer disposer)
   {
      ConstIterator itend(this->cend());
      ConstIterator cur(this->cbegin());
      SizeType n = 0;

      if(cur != itend){
         ConstIterator after(cur);
         ++after;
         while(after != itend){
            if(pred(*cur, *after)){
               after = this->erase_and_dispose(after, disposer);
               ++n;
            }
            else{
               cur = after;
               ++after;
            }
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
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeAlgorithms::inited(ValueTraits::to_node_ptr(value)));
      return Iterator(ValueTraits::to_node_ptr(value), ConstValueTraitsPtr());
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
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeAlgorithms::inited(ValueTraits::to_node_ptr(r)));
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
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeAlgorithms::inited(this->priv_value_traits().to_node_ptr(value)));
      return Iterator(this->priv_value_traits().to_node_ptr(value), this->priv_value_traits_ptr());
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
      Reference r = *detail::uncast(PointerTraits<ConstPointer>::pointer_to(value));
      BOOST_INTRUSIVE_INVARIANT_ASSERT(!NodeAlgorithms::inited(this->priv_value_traits().to_node_ptr(r)));
      return ConstIterator(this->priv_value_traits().to_node_ptr(r), this->priv_value_traits_ptr());
   }

   //! <b>Effects</b>: Asserts the integrity of the container.
   //!
   //! <b>Complexity</b>: Linear time.
   //!
   //! <b>Note</b>: The method has no effect when asserts are turned off (e.g., with NDEBUG).
   //!   Experimental function, interface might change in future versions.
   void check() const
   {
      ConstNodePtr header_ptr = get_root_node();
      // header's next and prev are never null
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_next(header_ptr));
      BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_previous(header_ptr));
      // header's next and prev either both point to header (empty list) or neither does
      BOOST_INTRUSIVE_INVARIANT_ASSERT((NodeTraits::get_next(header_ptr) == header_ptr)
         == (NodeTraits::get_previous(header_ptr) == header_ptr));
      if (NodeTraits::get_next(header_ptr) == header_ptr)
      {
         BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
            BOOST_INTRUSIVE_INVARIANT_ASSERT(this->priv_size_traits().get_size() == 0);
         return;
      }
      size_t node_count = 0; (void)node_count;
      ConstNodePtr p = header_ptr;
      while (true)
      {
         ConstNodePtr next_p = NodeTraits::get_next(p);
         BOOST_INTRUSIVE_INVARIANT_ASSERT(next_p);
         BOOST_INTRUSIVE_INVARIANT_ASSERT(NodeTraits::get_previous(next_p) == p);
         p = next_p;
         if (p == header_ptr) break;
         ++node_count;
      }
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
         BOOST_INTRUSIVE_INVARIANT_ASSERT(this->priv_size_traits().get_size() == node_count);
   }

   friend bool operator==(const ListImpl &x, const ListImpl &y)
   {
      BOOST_IF_CONSTEXPR(CONSTANT_TIME_SIZE)
      if(x.size() != y.size()){
         return false;
      }
      return ::boost::intrusive::algo_equal(x.cbegin(), x.cend(), y.cbegin(), y.cend());
   }

   inline friend bool operator!=(const ListImpl &x, const ListImpl &y)
   {  return !(x == y); }

   inline friend bool operator<(const ListImpl &x, const ListImpl &y)
   {  return ::boost::intrusive::algo_lexicographical_compare(x.begin(), x.end(), y.begin(), y.end());  }

   inline friend bool operator>(const ListImpl &x, const ListImpl &y)
   {  return y < x;  }

   inline friend bool operator<=(const ListImpl &x, const ListImpl &y)
   {  return !(y < x);  }

   inline friend bool operator>=(const ListImpl &x, const ListImpl &y)
   {  return !(x < y);  }

   inline friend void swap(ListImpl &x, ListImpl &y) BOOST_NOEXCEPT
   {  x.swap(y);  }

   /// @cond

   private:
   BOOST_INTRUSIVE_NO_DANGLING
   static ListImpl &priv_container_from_end_iterator(const ConstIterator &end_iterator) BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT((HAS_CONTAINER_FROM_ITERATOR));
      NodePtr p = end_iterator.pointed_node();
      HeaderHolderType* h = HeaderHolderType::get_holder(p);
      RootPlusSize* r = detail::parent_from_member
         < RootPlusSize, HeaderHolderType>(h, &RootPlusSize::m_header);
      DataT *d = detail::parent_from_member<DataT, RootPlusSize>
         ( r, &DataT::root_plus_size_);
      ListImpl *s  = detail::parent_from_member<ListImpl, DataT>(d, &ListImpl::m_data);
      return *s;
   }
   /// @endcond
};


//! Helper metafunction to define a \c list that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void, class O3 = void, class O4 = void>
#endif
struct MakeList
{
   /// @cond
   typedef typename PackOptions
      < ListDefaults,
         #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
         O1, O2, O3, O4
         #else
         Options...
         #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;
   typedef ListImpl
      <
         ValueTraits,
         typename PackedOptions::size_type,
         PackedOptions::constant_time_size,
         typename PackedOptions::header_holder_type
      > ImplementationDefined;
   /// @endcond
   typedef ImplementationDefined Type;
};


#ifndef BOOST_INTRUSIVE_DOXYGEN_INVOKED

#if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class O1, class O2, class O3, class O4>
#else
template<class T, class ...Options>
#endif
class List
   :  public MakeList<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
   >::type
{
   typedef typename MakeList
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4
      #else
      Options...
      #endif
      >::type      Base;
   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename Base::value_traits::value_type, T>::value));
   BOOST_MOVABLE_BUT_NOT_COPYABLE(List)

   public:
   typedef typename Base::ValueTraits          ValueTraits;
   typedef typename Base::Iterator              Iterator;
   typedef typename Base::ConstIterator        ConstIterator;

   inline List()
      :  Base()
   {}

   inline explicit List(const ValueTraits &v_traits)
      :  Base(v_traits)
   {}

   template<class Iterator>
   inline List(Iterator b, Iterator e, const ValueTraits &v_traits = ValueTraits())
      :  Base(b, e, v_traits)
   {}

   inline List(BOOST_RV_REF(List) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline List& operator=(BOOST_RV_REF(List) x)
   {  return static_cast<List &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const List &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(List) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static List &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<List &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const List &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const List &>(Base::container_from_end_iterator(end_iterator));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_LIST_HPP
