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
//
// The option that yields to non-floating point 1/sqrt(2) alpha is taken
// from the scapegoat tree implementation of the PSPP library.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_SGTREE_HPP
#define BOOST_INTRUSIVE_SGTREE_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/intrusive_fwd.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/bs_set_hook.hpp>
#include <boost/intrusive/bstree.hpp>
#include <boost/intrusive/detail/tree_node.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/math.hpp>
#include <boost/intrusive/detail/get_value_traits.hpp>
#include <boost/intrusive/sgtree_algorithms.hpp>
#include <boost/intrusive/detail/key_nodeptr_comp.hpp>
#include <boost/intrusive/link_mode.hpp>

#include <boost/move/utility_core.hpp>
#include <boost/move/adl_move_swap.hpp>

#include <cstddef>
#include <boost/intrusive/detail/minimal_less_equal_header.hpp>
#include <boost/intrusive/detail/minimal_pair_header.hpp>   //std::pair
#include <cstddef>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {

/// @cond

namespace detail{

/////////////////////////////////////////////////////////////
//
//       Halpha for fixed floating_point<false> option
//
/////////////////////////////////////////////////////////////

//! Returns floor(log2(n)/log2(sqrt(2))) -> floor(2*log2(n))
//! Undefined if N is 0.
//!
//! This function does not use float point operations.
inline std::size_t calculate_h_sqrt2 (std::size_t n)
{
   std::size_t f_log2 = detail::floor_log2(n);
   return (2*f_log2) + static_cast<std::size_t>(n >= detail::sqrt2_pow_2xplus1(f_log2));
}

struct HAlphaSqrt2T
{
   HAlphaSqrt2T(void){}
   std::size_t operator()(std::size_t n) const
   {  return calculate_h_sqrt2(n);  }
};

struct Alpha075ByMaxSizeT
{
   Alpha075ByMaxSizeT(void){}

   std::size_t operator()(std::size_t max_tree_size) const
   {
      const std::size_t max_tree_size_limit = ((~std::size_t(0))/std::size_t(3));
      return max_tree_size > max_tree_size_limit ? max_tree_size/4*3 : max_tree_size*3/4;
   }
};

/////////////////////////////////////////////////////////////
//
//       Halpha for fixed floating_point<true> option
//
/////////////////////////////////////////////////////////////

struct HAlphaT
{
   explicit HAlphaT(float inv_minus_logalpha)
      :  m_inv_minus_logalpha(inv_minus_logalpha)
   {}

   std::size_t operator()(std::size_t n) const
   {
      ////////////////////////////////////////////////////////////
      // This function must return "floor(log2(1/alpha(n)))" ->
      //    floor(log2(n)/log(1/alpha)) ->
      //    floor(log2(n)/-log2(alpha))
      //    floor(log2(n)*(1/-log2(alpha)))
      ////////////////////////////////////////////////////////////
      return static_cast<std::size_t>(detail::fast_log2(float(n))*m_inv_minus_logalpha);
   }

   private:
   //Since the function will be repeatedly called
   //precalculate constant data to avoid repeated
   //calls to log and division.
   //This will store 1/(-std::log2(alpha_))
   float m_inv_minus_logalpha;
};

struct AlphaByMaxSizeT
{
   explicit AlphaByMaxSizeT(float alpha)
      :  m_alpha(alpha)
   {}

   float operator()(std::size_t max_tree_size) const
   {  return float(max_tree_size)*m_alpha;   }

   private:
   float m_alpha;
};

template<bool Activate, class SizeType>
struct AlphaHolder
{
   typedef boost::intrusive::detail::HAlphaT           HAlphaT;
   typedef boost::intrusive::detail::AlphaByMaxSizeT MultiplyByAlphaT;

   AlphaHolder()
      : m_max_tree_size()
   {  set_alpha(0.70711f);   } // ~1/sqrt(2)

   [[nodiscard]] float get_alpha() const
   {  return m_alpha;  }

   void set_alpha(float alpha)
   {
      m_alpha = alpha;
      m_inv_minus_logalpha = 1/(-detail::fast_log2(alpha));
   }

   [[nodiscard]] HAlphaT get_h_alpha_t() const
   {  return HAlphaT(m_inv_minus_logalpha);  }

   [[nodiscard]] MultiplyByAlphaT get_multiply_by_alpha_t() const
   {  return MultiplyByAlphaT(m_alpha);  }

   SizeType &get_max_tree_size()
   {  return m_max_tree_size;  }

   protected:
   float m_alpha;
   float m_inv_minus_logalpha;
   SizeType m_max_tree_size;
};

template<class SizeType>
struct AlphaHolder<false, SizeType>
{
   //This specialization uses alpha = 1/sqrt(2)
   //without using floating point operations
   //Downside: alpha CAN't be changed.
   typedef boost::intrusive::detail::HAlphaSqrt2T           HAlphaT;
   typedef boost::intrusive::detail::Alpha075ByMaxSizeT  MultiplyByAlphaT;

   AlphaHolder()
      : m_max_tree_size()
   {}

   [[nodiscard]] float get_alpha() const
   {  return 0.70710677f;  }

   void set_alpha(float)
   {  //alpha CAN't be changed.
      BOOST_INTRUSIVE_INVARIANT_ASSERT(0);
   }

   [[nodiscard]] HAlphaT get_h_alpha_t() const
   {  return HAlphaT();  }

   [[nodiscard]] MultiplyByAlphaT get_multiply_by_alpha_t() const
   {  return MultiplyByAlphaT();  }

   SizeType &get_max_tree_size()
   {  return m_max_tree_size;  }

   protected:
   SizeType m_max_tree_size;
};

}  //namespace detail{

struct SgtreeDefaults
   : BstreeDefaults
{
   static const bool FLOATING_POINT = true;
};

/// @endcond

//! The class template sgtree is an intrusive scapegoat tree container, that
//! is used to construct intrusive sg_set and sg_multiset containers.
//! The no-throw guarantee holds only, if the value_compare object
//! doesn't throw.
//!
//! The template parameter \c T is the type to be managed by the container.
//! The user can specify additional options and if no options are provided
//! default options are used.
//!
//! The container supports the following options:
//! \c base_hook<>/member_hook<>/value_traits<>,
//! \c floating_point<>, \c size_type<> and
//! \c compare<>.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
template<class T, class ...Options>
#else
template<class TValueTraits, class VoidOrKeyOfValue, class VoidOrKeyComp, class TSizeType, bool FloatingPoint, typename HeaderHolder>
#endif
class SgtreeImpl
   /// @cond
   :  public BstreeImpl<TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType, true, SgTreeAlgorithmsValue, HeaderHolder>
   ,  public detail::AlphaHolder<FloatingPoint, TSizeType>
   /// @endcond
{
   public:
   typedef TValueTraits                                               ValueTraits;
   /// @cond
   typedef BstreeImpl< TValueTraits, VoidOrKeyOfValue, VoidOrKeyComp, TSizeType
                      , true, SgTreeAlgorithmsValue, HeaderHolder>        TreeType;
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
   typedef BOOST_INTRUSIVE_IMPDEF(SgtreeAlgorithms<NodeTraits>)    NodeAlgorithms;

   static const bool CONSTANT_TIME_SIZE      = ImplementationDefined::CONSTANT_TIME_SIZE;
   static const bool FLOATING_POINT          = FloatingPoint;
   static const bool STATEFUL_VALUE_TRAITS   = ImplementationDefined::STATEFUL_VALUE_TRAITS;

   /// @cond
   private:

   //noncopyable
   typedef detail::AlphaHolder<FloatingPoint, TSizeType>    AlphaTraits;
   typedef typename AlphaTraits::HAlphaT                 HAlphaT;
   typedef typename AlphaTraits::MultiplyByAlphaT       MultiplyByAlphaT;

   BOOST_MOVABLE_BUT_NOT_COPYABLE(SgtreeImpl)
   BOOST_INTRUSIVE_STATIC_ASSERT(((int)ValueTraits::link_mode != (int)AutoUnlink));

   enum { SafemodeOrAutounlink  =
            (int)ValueTraits::link_mode == (int)AutoUnlink   ||
            (int)ValueTraits::link_mode == (int)SafeLink     };

   /// @endcond

   public:

   typedef BOOST_INTRUSIVE_IMPDEF(typename NodeAlgorithms::InsertCommitData) InsertCommitData;

   //! @copydoc ::boost::intrusive::bstree::bstree()
   SgtreeImpl()
      :  TreeType()
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(const key_compare &,const value_traits &)
   explicit SgtreeImpl( const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  TreeType(cmp, v_traits)
   {}

   //! @copydoc ::boost::intrusive::bstree::bstree(bool,Iterator,Iterator,const key_compare &,const value_traits &)
   template<class Iterator>
   SgtreeImpl( bool unique, Iterator b, Iterator e
              , const KeyCompare &cmp     = KeyCompare()
              , const ValueTraits &v_traits = ValueTraits())
      : TreeType(cmp, v_traits)
   {
      if(unique)
         this->insert_unique(b, e);
      else
         this->insert_equal(b, e);
   }

   //! @copydoc ::boost::intrusive::bstree::bstree(bstree &&)
   SgtreeImpl(BOOST_RV_REF(SgtreeImpl) x)
      :  TreeType(BOOST_MOVE_BASE(TreeType, x)), AlphaTraits(x.get_alpha_traits())
   {  ::boost::adl_move_swap(this->get_alpha_traits(), x.get_alpha_traits());   }

   //! @copydoc ::boost::intrusive::bstree::operator=(bstree &&)
   SgtreeImpl& operator=(BOOST_RV_REF(SgtreeImpl) x)
   {
      this->get_alpha_traits() = x.get_alpha_traits();
      return static_cast<SgtreeImpl&>(TreeType::operator=(BOOST_MOVE_BASE(TreeType, x)));
   }

   /// @cond
   private:

   const AlphaTraits &get_alpha_traits() const
   {  return *this;  }

   AlphaTraits &get_alpha_traits()
   {  return *this;  }

   HAlphaT get_h_alpha_func() const
   {  return this->get_alpha_traits().get_h_alpha_t();  }

   MultiplyByAlphaT get_alpha_by_max_size_func() const
   {  return this->get_alpha_traits().get_multiply_by_alpha_t(); }

   /// @endcond

   public:

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
   //! @copydoc ::boost::intrusive::bstree::~bstree()
   ~SgtreeImpl();

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
   static SgtreeImpl &container_from_end_iterator(iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_end_iterator(const_iterator)
   static const SgtreeImpl &container_from_end_iterator(const_iterator end_iterator) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(iterator)
   static SgtreeImpl &container_from_iterator(iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::container_from_iterator(const_iterator)
   static const SgtreeImpl &container_from_iterator(const_iterator it) BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::key_comp()const
   key_compare key_comp() const;

   //! @copydoc ::boost::intrusive::bstree::value_comp()const
   value_compare value_comp() const;

   //! @copydoc ::boost::intrusive::bstree::empty()const
   bool empty() const BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::size()const
   size_type size() const BOOST_NOEXCEPT;

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! @copydoc ::boost::intrusive::bstree::swap
   void swap(SgtreeImpl& other)
   {
      //This can throw
      this->TreeType::swap(static_cast<TreeType&>(other));
      ::boost::adl_move_swap(this->get_alpha_traits(), other.get_alpha_traits());
   }

   //! @copydoc ::boost::intrusive::bstree::clone_from(const bstree&,Cloner,Disposer)
   //! Additional notes: it also copies the alpha factor from the source container.
   template <class Cloner, class Disposer>
   void clone_from(const SgtreeImpl &src, Cloner cloner, Disposer disposer)
   {
      TreeType::clone_from(src, cloner, disposer);
      this->get_alpha_traits() = src.get_alpha_traits();
   }

   //! @copydoc ::boost::intrusive::bstree::clone_from(bstree&&,Cloner,Disposer)
   //! Additional notes: it also copies the alpha factor from the source container.
   template <class Cloner, class Disposer>
   void clone_from(BOOST_RV_REF(SgtreeImpl) src, Cloner cloner, Disposer disposer)
   {
      TreeType::clone_from(BOOST_MOVE_BASE(TreeType, src), cloner, disposer);
      this->get_alpha_traits() = ::boost::move(src.get_alpha_traits());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(reference)
   Iterator insert_equal(Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodePtr p = NodeAlgorithms::insert_equal_upper_bound
         (this->TreeType::header_ptr(), to_insert, this->key_node_comp(this->key_comp())
         , (SizeType)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
      return Iterator(p, this->priv_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(const_iterator,reference)
   Iterator insert_equal(ConstIterator hint, Reference value)
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodePtr p = NodeAlgorithms::insert_equal
         ( this->TreeType::header_ptr(), hint.pointed_node(), to_insert, this->key_node_comp(this->key_comp())
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
      return Iterator(p, this->priv_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_equal(Iterator,Iterator)
   template<class Iterator>
   void insert_equal(Iterator b, Iterator e)
   {
      Iterator iend(this->end());
      for (; b != e; ++b)
         this->insert_equal(iend, *b);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(reference)
   std::pair<Iterator, bool> insert_unique(Reference value)
   {
      InsertCommitData commit_data;
      std::pair<Iterator, bool> ret = this->insert_unique_check
         (KeyOfValue()(value), this->key_comp(), commit_data);
      if(!ret.second)
         return ret;
      return std::pair<Iterator, bool> (this->insert_unique_commit(value, commit_data), true);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(const_iterator,reference)
   Iterator insert_unique(ConstIterator hint, Reference value)
   {
      InsertCommitData commit_data;
      std::pair<Iterator, bool> ret = this->insert_unique_check
         (hint, KeyOfValue()(value), this->key_comp(), commit_data);
      if(!ret.second)
         return ret.first;
      return this->insert_unique_commit(value, commit_data);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   BOOST_INTRUSIVE_DOC1ST(std::pair<iterator BOOST_INTRUSIVE_I bool>
      , typename detail::disable_if_convertible
         <KeyType BOOST_INTRUSIVE_I ConstIterator BOOST_INTRUSIVE_I 
         std::pair<Iterator BOOST_INTRUSIVE_I bool> >::type)
      insert_unique_check
      (const KeyType &key, KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         NodeAlgorithms::insert_unique_check
            (this->TreeType::header_ptr(), key, this->key_node_comp(comp), commit_data);
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const KeyType&,KeyTypeKeyCompare,insert_commit_data&)
   template<class KeyType, class KeyTypeKeyCompare>
   std::pair<Iterator, bool> insert_unique_check
      (ConstIterator hint, const KeyType &key
      ,KeyTypeKeyCompare comp, InsertCommitData &commit_data)
   {
      std::pair<NodePtr, bool> ret =
         NodeAlgorithms::insert_unique_check
            (this->TreeType::header_ptr(), hint.pointed_node(), key, this->key_node_comp(comp), commit_data);
      return std::pair<Iterator, bool>(Iterator(ret.first, this->priv_value_traits_ptr()), ret.second);
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_unique_check
      (const KeyType &key, InsertCommitData &commit_data)
   {  return this->insert_unique_check(key, this->key_comp(), commit_data);   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_check(const_iterator,const key_type&,insert_commit_data&)
   std::pair<Iterator, bool> insert_unique_check
      (ConstIterator hint, const KeyType &key, InsertCommitData &commit_data)
   {  return this->insert_unique_check(hint, key, this->key_comp(), commit_data);   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique_commit
   Iterator insert_unique_commit(Reference value, const InsertCommitData &commit_data) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodeAlgorithms::insert_unique_commit
         ( this->TreeType::header_ptr(), to_insert, commit_data
         , (std::size_t)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
      return Iterator(to_insert, this->priv_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::insert_unique(Iterator,Iterator)
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

   //! @copydoc ::boost::intrusive::bstree::insert_before
   Iterator insert_before(ConstIterator pos, Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodePtr p = NodeAlgorithms::insert_before
         ( this->TreeType::header_ptr(), pos.pointed_node(), to_insert
         , (SizeType)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
      return Iterator(p, this->priv_value_traits_ptr());
   }

   //! @copydoc ::boost::intrusive::bstree::push_back
   void push_back(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodeAlgorithms::push_back
         ( this->TreeType::header_ptr(), to_insert
         , (SizeType)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
   }

   //! @copydoc ::boost::intrusive::bstree::push_front
   void push_front(Reference value) BOOST_NOEXCEPT
   {
      NodePtr to_insert(this->get_value_traits().to_node_ptr(value));
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || NodeAlgorithms::unique(to_insert));
      std::size_t max_tree_size = (std::size_t)this->max_tree_size_;
      NodeAlgorithms::push_front
         ( this->TreeType::header_ptr(), to_insert
         , (SizeType)this->size(), this->get_h_alpha_func(), max_tree_size);
      this->TreeType::sz_traits().increment();
      this->max_tree_size_ = (SizeType)max_tree_size;
   }


   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator)
   Iterator erase(ConstIterator i) BOOST_NOEXCEPT
   {
      ConstIterator ret(i);
      ++ret;
      NodePtr to_erase(i.pointed_node());
      BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || !NodeAlgorithms::unique(to_erase));
      std::size_t max_tree_size = this->max_tree_size_;
      NodeAlgorithms::erase
         ( this->TreeType::header_ptr(), to_erase, (std::size_t)this->size()
         , max_tree_size, this->get_alpha_by_max_size_func());
      this->max_tree_size_ = (SizeType)max_tree_size;
      this->TreeType::sz_traits().decrement();
      BOOST_IF_CONSTEXPR(SafemodeOrAutounlink)
         NodeAlgorithms::init(to_erase);
      return ret.unconst();
   }

   //! @copydoc ::boost::intrusive::bstree::erase(const_iterator,const_iterator)
   Iterator erase(ConstIterator b, ConstIterator e) BOOST_NOEXCEPT
   {  SizeType n;   return private_erase(b, e, n);   }

   //! @copydoc ::boost::intrusive::bstree::erase(const key_type &)
   SizeType erase(const KeyType &key)
   {  return this->erase(key, this->key_comp());   }

   //! @copydoc ::boost::intrusive::bstree::erase(const KeyType&,KeyTypeKeyCompare)
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

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,Disposer)
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

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const_iterator,const_iterator,Disposer)
   template<class Disposer>
   Iterator erase_and_dispose(ConstIterator b, ConstIterator e, Disposer disposer) BOOST_NOEXCEPT
   {  SizeType n;   return private_erase(b, e, n, disposer);   }

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const key_type &, Disposer)
   template<class Disposer>
   SizeType erase_and_dispose(const KeyType &key, Disposer disposer)
   {
      std::pair<Iterator,Iterator> p = this->equal_range(key);
      SizeType n;
      private_erase(p.first, p.second, n, disposer);
      return n;
   }

   //! @copydoc ::boost::intrusive::bstree::erase_and_dispose(const KeyType&,KeyTypeKeyCompare,Disposer)
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

   //! @copydoc ::boost::intrusive::bstree::clear
   void clear() BOOST_NOEXCEPT
   {
      TreeType::clear();
      this->max_tree_size_ = 0;
   }

   //! @copydoc ::boost::intrusive::bstree::clear_and_dispose
   template<class Disposer>
   void clear_and_dispose(Disposer disposer) BOOST_NOEXCEPT
   {
      TreeType::clear_and_dispose(disposer);
      this->max_tree_size_ = 0;
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! @copydoc ::boost::intrusive::bstree::merge_unique
   template<class T, class ...Options2> void merge_unique(sgtree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_unique(SgtreeImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, FloatingPoint, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);

         std::size_t max_tree1_size = this->max_tree_size_;
         std::size_t max_tree2_size = source.get_max_tree_size();
         if( NodeAlgorithms::transfer_unique
               ( this->header_ptr(), this->key_node_comp(this->key_comp()), this->size(), max_tree1_size
               , source.header_ptr(), p, source.size(), max_tree2_size
               , this->get_h_alpha_func(), this->get_alpha_by_max_size_func()) ){
            this->max_tree_size_  = (SizeType)max_tree1_size;
            this->sz_traits().increment();
            source.get_max_tree_size() = (SizeType)max_tree2_size;
            source.sz_traits().decrement();
         }
      }
   }

   #if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED)
   //! @copydoc ::boost::intrusive::bstree::merge_equal
   template<class T, class ...Options2> void merge_equal(sgtree<T, Options2...> &);
   #else
   template<class Compare2>
   void merge_equal(SgtreeImpl
      <TValueTraits, VoidOrKeyOfValue, Compare2, TSizeType, FloatingPoint, HeaderHolder> &source)
   #endif
   {
      NodePtr it   (NodeAlgorithms::begin_node(source.header_ptr()))
             , itend(NodeAlgorithms::end_node  (source.header_ptr()));

      while(it != itend){
         NodePtr const p(it);
         BOOST_INTRUSIVE_SAFE_HOOK_DEFAULT_ASSERT(!SafemodeOrAutounlink || !NodeAlgorithms::unique(p));
         it = NodeAlgorithms::next_node(it);
         std::size_t max_tree1_size = this->max_tree_size_;
         std::size_t max_tree2_size = source.get_max_tree_size();
         NodeAlgorithms::transfer_equal
            ( this->header_ptr(), this->key_node_comp(this->key_comp()), this->size(), max_tree1_size
            , source.header_ptr(), p, source.size(), max_tree2_size
            , this->get_h_alpha_func(), this->get_alpha_by_max_size_func());
         this->max_tree_size_  = (SizeType)max_tree1_size;
         this->sz_traits().increment();
         source.get_max_tree_size() = (SizeType)max_tree2_size;
         source.sz_traits().decrement();
      }
   }

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

   //! @copydoc ::boost::intrusive::bstree::rebalance
   void rebalance() BOOST_NOEXCEPT;

   //! @copydoc ::boost::intrusive::bstree::rebalance_subtree
   iterator rebalance_subtree(iterator root) BOOST_NOEXCEPT;

   friend bool operator< (const SgtreeImpl &x, const SgtreeImpl &y);

   friend bool operator==(const SgtreeImpl &x, const SgtreeImpl &y);

   friend bool operator!= (const SgtreeImpl &x, const SgtreeImpl &y);

   friend bool operator>(const SgtreeImpl &x, const SgtreeImpl &y);

   friend bool operator<=(const SgtreeImpl &x, const SgtreeImpl &y);

   friend bool operator>=(const SgtreeImpl &x, const SgtreeImpl &y);

   friend void swap(SgtreeImpl &x, SgtreeImpl &y);

   #endif   //#ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED

   //! <b>Returns</b>: The balance factor (alpha) used in this tree
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Constant.
   [[nodiscard]] float balance_factor() const BOOST_NOEXCEPT
   {  return this->get_alpha_traits().get_alpha(); }

   //! <b>Requires</b>: new_alpha must be a value between 0.5 and 1.0
   //!
   //! <b>Effects</b>: Establishes a new balance factor (alpha) and rebalances
   //!   the tree if the new balance factor is stricter (less) than the old factor.
   //!
   //! <b>Throws</b>: Nothing.
   //!
   //! <b>Complexity</b>: Linear to the elements in the subtree.
   void balance_factor(float new_alpha) BOOST_NOEXCEPT
   {
      //The alpha factor CAN't be changed if the fixed, floating operation-less
      //1/sqrt(2) alpha factor option is activated
      BOOST_INTRUSIVE_STATIC_ASSERT((FLOATING_POINT));
      BOOST_INTRUSIVE_INVARIANT_ASSERT((new_alpha > 0.5f && new_alpha < 1.0f));
      if(new_alpha >= 0.5f && new_alpha < 1.0f){
         float old_alpha = this->get_alpha_traits().get_alpha();
         this->get_alpha_traits().set_alpha(new_alpha);
         if(new_alpha < old_alpha){
            this->max_tree_size_ = this->size();
            this->rebalance();
         }
      }
   }

   /// @cond
   private:
   template<class Disposer>
   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n, Disposer disposer) BOOST_NOEXCEPT
   {
      for(n = 0; b != e; ++n)
        this->erase_and_dispose(b++, disposer);
      return b.unconst();
   }

   Iterator private_erase(ConstIterator b, ConstIterator e, SizeType &n) BOOST_NOEXCEPT
   {
      for(n = 0; b != e; ++n)
        this->erase(b++);
      return b.unconst();
   }
   /// @endcond
};


//! Helper metafunction to define a \c sgtree that yields to the same type when the
//! same options (either explicitly or implicitly) are used.
#if defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) || defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
template<class T, class ...Options>
#else
template<class T, class O1 = void, class O2 = void
                , class O3 = void, class O4 = void
                , class O5 = void, class O6 = void>
#endif
struct MakeSgtree
{
   /// @cond
   typedef typename PackOptions
      < SgtreeDefaults,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type PackedOptions;

   typedef typename detail::GetValueTraits
      <T, typename PackedOptions::proto_value_traits>::type ValueTraits;

   typedef SgtreeImpl
         < ValueTraits
         , typename PackedOptions::key_of_value
         , typename PackedOptions::compare
         , typename PackedOptions::size_type
         , PackedOptions::floating_point
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
class Sgtree
   :  public MakeSgtree<T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type
{
   typedef typename MakeSgtree
      <T,
      #if !defined(BOOST_INTRUSIVE_VARIADIC_TEMPLATES)
      O1, O2, O3, O4, O5, O6
      #else
      Options...
      #endif
      >::type   Base;
   BOOST_MOVABLE_BUT_NOT_COPYABLE(Sgtree)

   public:
   typedef typename Base::KeyCompare        KeyCompare;
   typedef typename Base::ValueTraits       ValueTraits;
   typedef typename Base::Iterator           Iterator;
   typedef typename Base::ConstIterator     ConstIterator;
   typedef typename Base::ReverseIterator           ReverseIterator;
   typedef typename Base::ConstReverseIterator     ConstReverseIterator;

   //Assert if passed value traits are compatible with the type
   BOOST_INTRUSIVE_STATIC_ASSERT((detail::is_same<typename ValueTraits::value_type, T>::value));

   inline Sgtree()
      :  Base()
   {}

   inline explicit Sgtree(const KeyCompare &cmp, const ValueTraits &v_traits = ValueTraits())
      :  Base(cmp, v_traits)
   {}

   template<class Iterator>
   inline Sgtree( bool unique, Iterator b, Iterator e
         , const KeyCompare &cmp = KeyCompare()
         , const ValueTraits &v_traits = ValueTraits())
      :  Base(unique, b, e, cmp, v_traits)
   {}

   inline Sgtree(BOOST_RV_REF(Sgtree) x)
      :  Base(BOOST_MOVE_BASE(Base, x))
   {}

   inline Sgtree& operator=(BOOST_RV_REF(Sgtree) x)
   {  return static_cast<Sgtree &>(this->Base::operator=(BOOST_MOVE_BASE(Base, x)));  }

   template <class Cloner, class Disposer>
   inline void clone_from(const Sgtree &src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(src, cloner, disposer);  }

   template <class Cloner, class Disposer>
   inline void clone_from(BOOST_RV_REF(Sgtree) src, Cloner cloner, Disposer disposer)
   {  Base::clone_from(BOOST_MOVE_BASE(Base, src), cloner, disposer);  }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Sgtree &container_from_end_iterator(Iterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<Sgtree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Sgtree &container_from_end_iterator(ConstIterator end_iterator) BOOST_NOEXCEPT
   {  return static_cast<const Sgtree &>(Base::container_from_end_iterator(end_iterator));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static Sgtree &container_from_iterator(Iterator it) BOOST_NOEXCEPT
   {  return static_cast<Sgtree &>(Base::container_from_iterator(it));   }

   BOOST_INTRUSIVE_NO_DANGLING
   inline static const Sgtree &container_from_iterator(ConstIterator it) BOOST_NOEXCEPT
   {  return static_cast<const Sgtree &>(Base::container_from_iterator(it));   }
};

#endif

} //namespace intrusive
} //namespace boost

#include <boost/intrusive/detail/config_end.hpp>

#endif //BOOST_INTRUSIVE_SGTREE_HPP
