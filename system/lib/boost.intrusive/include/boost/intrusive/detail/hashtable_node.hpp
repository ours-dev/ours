/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2007-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_HASHTABLE_NODE_HPP
#define BOOST_INTRUSIVE_HASHTABLE_NODE_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/trivial_value_traits.hpp>
#include <boost/intrusive/detail/common_slist_algorithms.hpp>
#include <boost/intrusive/detail/iiterator.hpp>
#include <boost/intrusive/detail/slist_iterator.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>
#include <cstddef>
#include <climits>
#include <boost/move/core.hpp>


namespace boost {
namespace intrusive {

template <class TNodeTraits>
struct BucketImpl
   : public TNodeTraits::node
{
   public:
   typedef TNodeTraits NodeTraits;

   private:
   typedef typename NodeTraits::NodePtr          NodePtr;
   typedef typename NodeTraits::ConstNodePtr    ConstNodePtr;

   typedef detail::CommonSlistAlgorithms<TNodeTraits> AlgoT;

   public:
   inline BucketImpl()
   {}

   inline BucketImpl(const BucketImpl &)
   {}

   inline ~BucketImpl()
   {}

   inline BucketImpl &operator=(const BucketImpl&)
   {  return *this;  }

   inline NodePtr get_node_ptr()
   {  return PointerTraits<NodePtr>::pointer_to(*this);  }

   inline ConstNodePtr get_node_ptr() const
   {  return PointerTraits<ConstNodePtr>::pointer_to(*this);  }

   inline NodePtr begin_ptr()
   {  return NodeTraits::get_next(get_node_ptr());  }
};



template <class NodeTraits>
struct HashReducedSlistNodeTraits
{
   template <class U> static detail::no_type test(...);
   template <class U> static detail::yes_type test(typename U::reduced_slist_node_traits*);
   static const bool VALUE = sizeof(test<NodeTraits>(0)) == sizeof(detail::yes_type);
};

template <class NodeTraits>
struct ApplyReducedSlistNodeTraits
{
   typedef typename NodeTraits::reduced_slist_node_traits Type;
};

template <class NodeTraits>
struct ReducedSlistNodeTraits
{
   typedef typename detail::eval_if_c
      < HashReducedSlistNodeTraits<NodeTraits>::value
      , ApplyReducedSlistNodeTraits<NodeTraits>
      , detail::identity<NodeTraits>
      >::Type Type;
};

template<class BucketValueTraits, bool LinearBuckets, bool IsConst>
class HashtableIterator
{
   typedef typename BucketValueTraits::ValueTraits            ValueTraits;
   typedef typename BucketValueTraits::BucketTraits           BucketTraits;

   typedef Iiterator< ValueTraits, IsConst
                    , std::forward_iterator_tag>   TypesT;
   public:
   typedef typename TypesT::iterator_type::DifferenceType    DifferenceType;
   typedef typename TypesT::iterator_type::ValueType         ValueType;
   typedef typename TypesT::iterator_type::Pointer            Pointer;
   typedef typename TypesT::iterator_type::Reference          Reference;
   typedef typename TypesT::iterator_type::IteratorCategory  IteratorCategory;

   private:
   typedef typename ValueTraits::NodeTraits                  NodeTraits;
   typedef typename NodeTraits::NodePtr                      NodePtr;
   typedef typename BucketValueTraits::BucketType             BucketType;
   typedef typename BucketType::node_traits                   SlistNodeTraits;
   typedef typename SlistNodeTraits::node_ptr                SlistNodePtr;
   typedef TrivialValueTraits
      <SlistNodeTraits, NormalLink>                         SlistValueTraits;
   typedef SlistIterator<SlistValueTraits, false>           Siterator;
   typedef SlistIterator<SlistValueTraits, true>            ConstSiterator;
   typedef CircularSlistAlgorithms<SlistNodeTraits>        SlistNodeAlgorithms;

   typedef typename PointerTraits
      <Pointer>::template rebind_pointer
         < const BucketValueTraits >::type                     ConstBucketvaltraitsPtr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , HashtableIterator<BucketValueTraits, LinearBuckets, false>
                  , nat>::type                                 NonconstIterator;

   inline static NodePtr downcast_bucket(typename BucketType::node_traits::node_ptr p)
   {
      return PointerTraits<NodePtr>::
         pointer_to(static_cast<typename NodeTraits::node&>(*p));
   }

   public:

   inline HashtableIterator ()
      : m_slist_it()  //Value initialization to achieve "null iterators" (N3644)
   {}

   inline explicit HashtableIterator(Siterator ptr, const BucketValueTraits *cont)
      : m_slist_it (ptr)
      , m_traitsptr (cont ? PointerTraits<ConstBucketvaltraitsPtr>::pointer_to(*cont) : ConstBucketvaltraitsPtr() )
   {}

   inline HashtableIterator(const HashtableIterator &other)
      :  m_slist_it(other.slist_it()), m_traitsptr(other.get_bucket_value_traits())
   {}

   inline HashtableIterator(const NonconstIterator &other)
      :  m_slist_it(other.slist_it()), m_traitsptr(other.get_bucket_value_traits())
   {}

   inline const Siterator &slist_it() const
   { return m_slist_it; }

   inline HashtableIterator<BucketValueTraits, LinearBuckets, false> unconst() const
   {  return HashtableIterator<BucketValueTraits, LinearBuckets, false>(this->slist_it(), this->get_bucket_value_traits());   }

   inline HashtableIterator& operator++()
   {  this->increment();   return *this;   }

   inline HashtableIterator &operator=(const HashtableIterator &other)
   {  m_slist_it = other.slist_it(); m_traitsptr = other.get_bucket_value_traits();   return *this;  }

   inline HashtableIterator operator++(int)
   {
      HashtableIterator result (*this);
      this->increment();
      return result;
   }

   inline friend bool operator== (const HashtableIterator& i, const HashtableIterator& i2)
   { return i.m_slist_it == i2.m_slist_it; }

   inline friend bool operator!= (const HashtableIterator& i, const HashtableIterator& i2)
   { return !(i == i2); }

   inline Reference operator*() const
   { return *this->operator ->(); }

   inline Pointer operator->() const
   {
      return this->priv_value_traits().to_value_ptr
         (downcast_bucket(m_slist_it.pointed_node()));
   }

   inline ConstBucketvaltraitsPtr get_bucket_value_traits() const
   {  return m_traitsptr;  }

   inline const ValueTraits &priv_value_traits() const
   {  return m_traitsptr->priv_value_traits();  }

   private:

   void increment()
   {
      BucketType* const buckets = boost::movelib::to_raw_pointer(m_traitsptr->priv_bucket_traits().bucket_begin());
      const std::size_t buckets_len = m_traitsptr->priv_bucket_traits().bucket_count();

      ++m_slist_it;
      const SlistNodePtr n = m_slist_it.pointed_node();
      const Siterator first_bucket_bbegin(buckets->get_node_ptr());
      if(first_bucket_bbegin.pointed_node() <= n && n <= buckets[buckets_len-1].get_node_ptr()){
         //If one-past the node is inside the bucket then look for the next non-empty bucket
         //1. get the bucket_impl from the iterator
         const BucketType &b = static_cast<const BucketType&>(*n);

         //2. Now just calculate the index b has in the bucket array
         std::size_t n_bucket = static_cast<std::size_t>(&b - buckets);

         //3. Iterate until a non-empty bucket is found
         SlistNodePtr bucket_nodeptr = buckets->get_node_ptr();
         do{
            if (++n_bucket >= buckets_len){  //bucket overflow, return end() iterator
               m_slist_it = first_bucket_bbegin;
               return;
            }
            bucket_nodeptr = buckets[n_bucket].get_node_ptr();
         }
         while (SlistNodeAlgorithms::is_empty(bucket_nodeptr));
         m_slist_it = Siterator(bucket_nodeptr);
         ++m_slist_it;
      }
      else{
         //++slist_it_ yield to a valid object
      }
   }

   Siterator                  m_slist_it;
   ConstBucketvaltraitsPtr  m_traitsptr;
};

template<class BucketValueTraits, bool IsConst>
class HashtableIterator<BucketValueTraits, true, IsConst>
{
   typedef typename BucketValueTraits::ValueTraits            ValueTraits;
   typedef typename BucketValueTraits::BucketTraits           BucketTraits;

   typedef Iiterator< ValueTraits, IsConst
                    , std::forward_iterator_tag>   TypesT;
   public:
   typedef typename TypesT::iterator_type::DifferenceType    DifferenceType;
   typedef typename TypesT::iterator_type::ValueType         ValueType;
   typedef typename TypesT::iterator_type::Pointer            Pointer;
   typedef typename TypesT::iterator_type::Reference          Reference;
   typedef typename TypesT::iterator_type::IteratorCategory  IteratorCategory;

   private:
   typedef typename ValueTraits::NodeTraits                  NodeTraits;
   typedef typename NodeTraits::NodePtr                      NodePtr;
   typedef typename BucketValueTraits::BucketType             BucketType;
   typedef typename BucketValueTraits::BucketPtr              BucketPtr;
   typedef typename BucketType::node_traits                   SlistNodeTraits;
   typedef LinearSlistAlgorithms<SlistNodeTraits>          SlistNodeAlgorithms;
   typedef typename SlistNodeTraits::node_ptr                SlistNodePtr;
   typedef TrivialValueTraits
      <SlistNodeTraits, NormalLink>                         SlistValueTraits;
   typedef SlistIterator<SlistValueTraits, false>           Siterator;
   typedef SlistIterator<SlistValueTraits, true>            ConstSiterator;

   static const bool STATEFUL_VALUE_TRAITS =
      detail::IsStatefulValueTraits<ValueTraits>::value;

   typedef typename PointerTraits
      <Pointer>::template rebind_pointer
         < const ValueTraits >::type                          ConstValueTraitsPtr;
   class nat;
   typedef typename
      detail::if_c< IsConst
                  , HashtableIterator<BucketValueTraits, true, false>
                  , nat>::type                                 NonconstIterator;

   inline static NodePtr downcast_bucket(SlistNodePtr p)
   {
      return PointerTraits<NodePtr>::
         pointer_to(static_cast<typename NodeTraits::node&>(*p));
   }

   public:

   inline HashtableIterator ()
      : m_slist_it()  //Value initialization to achieve "null iterators" (N3644)
      , m_members()
   {}

   inline explicit HashtableIterator(Siterator ptr, BucketPtr bp, ConstValueTraitsPtr traits_ptr)
      : m_slist_it (ptr)
      , m_members (bp, traits_ptr)
   {}

   inline HashtableIterator(const HashtableIterator &other)
      :  m_slist_it(other.slist_it()), m_members(other.get_bucket_ptr(), other.get_value_traits())
   {}

   inline HashtableIterator(const NonconstIterator &other)
      :  m_slist_it(other.slist_it()), m_members(other.get_bucket_ptr(), other.get_value_traits())
   {}

   inline const Siterator &slist_it() const
   { return m_slist_it; }

   inline HashtableIterator<BucketValueTraits, true, false> unconst() const
   {  return HashtableIterator<BucketValueTraits, true, false>(this->slist_it(), m_members.nodeptr_, m_members.get_ptr());   }

   inline HashtableIterator& operator++()
   {  this->increment();   return *this;   }

   inline HashtableIterator &operator=(const HashtableIterator &other)
   {  m_slist_it = other.slist_it(); m_members = other.m_members;  return *this;  }

   inline HashtableIterator operator++(int)
   {
      HashtableIterator result (*this);
      this->increment();
      return result;
   }

   inline friend bool operator== (const HashtableIterator& i, const HashtableIterator& i2)
   { return i.m_slist_it == i2.m_slist_it; }

   inline friend bool operator!= (const HashtableIterator& i, const HashtableIterator& i2)
   { return i.m_slist_it != i2.m_slist_it; }

   inline Reference operator*() const
   { return *this->operator ->(); }

   inline Pointer operator->() const
   { return this->operator_arrow(detail::bool_<STATEFUL_VALUE_TRAITS>()); }

   inline ConstValueTraitsPtr get_value_traits() const
   {  return m_members.get_ptr(); }

   inline BucketPtr get_bucket_ptr() const
   {  return m_members.nodeptr_; }

   private:

   inline Pointer operator_arrow(detail::false_) const
   { return ValueTraits::to_value_ptr(downcast_bucket(m_slist_it.pointed_node())); }

   inline Pointer operator_arrow(detail::true_) const
   { return this->get_value_traits()->to_value_ptr(downcast_bucket(m_slist_it.pointed_node())); }

   void increment()
   {
      ++m_slist_it;
      if (m_slist_it == Siterator()){
         SlistNodePtr bucket_nodeptr;
         do {
            ++m_members.nodeptr_;
             bucket_nodeptr = m_members.nodeptr_->get_node_ptr();
         }while(SlistNodeAlgorithms::is_empty(bucket_nodeptr));
         m_slist_it = Siterator(SlistNodeTraits::get_next(bucket_nodeptr));
      }
   }

   Siterator   m_slist_it;
   IiteratorMembers<BucketPtr, ConstValueTraitsPtr, STATEFUL_VALUE_TRAITS> m_members;
};

}  //namespace intrusive {
}  //namespace boost {

#endif
