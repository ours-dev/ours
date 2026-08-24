/////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga  2006-2014
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
/////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_HOOK_TRAITS_HPP
#define BOOST_INTRUSIVE_DETAIL_HOOK_TRAITS_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/detail/parent_from_member.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/move/detail/to_raw_pointer.hpp>
#include <boost/intrusive/detail/node_holder.hpp>

namespace boost {
namespace intrusive {

template<class T, class TNodePtr, class Tag, unsigned int Type>
struct BhtraitsBase
{
   public:
   typedef TNodePtr                                                   NodePtr;
   typedef typename PointerTraits<NodePtr>::element_type           Node;
   typedef NodeHolder<Node, Tag, Type>                              NodeHolderType;
   typedef T                                                         ValueType;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const Node>::type                      ConstNodePtr;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<T>::type                               Pointer;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const T>::type                         ConstPointer;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<NodeHolderType>::type                NodeHolderPtr;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const NodeHolderType>::type          ConstNodeHolderPtr;
   typedef T &                                                       Reference;
   typedef const T &                                                 ConstReference;
   typedef NodeHolderType &                                        NodeHolderReference;
   typedef const NodeHolderType &                                  ConstNodeHolderReference;
   typedef Node&                                                     NodeReference;
   typedef const Node &                                              ConstNodeReference;

   inline static Pointer to_value_ptr(NodePtr n)
   {
      return PointerTraits<Pointer>::
         static_cast_from(PointerTraits<NodeHolderPtr>::static_cast_from(n));
   }

   inline static ConstPointer to_value_ptr(ConstNodePtr n)
   {
      return PointerTraits<ConstPointer>::
         static_cast_from(PointerTraits<ConstNodeHolderPtr>::static_cast_from(n));
   }

   inline static NodePtr to_node_ptr(Reference value)
   {
      NodePtr p = PointerTraits<NodePtr>::pointer_to
         (static_cast<NodeReference>(static_cast<NodeHolderReference>(value)));
      return p;
   }

   inline static ConstNodePtr to_node_ptr(ConstReference value)
   {
      ConstNodePtr p = PointerTraits<ConstNodePtr>::pointer_to
         (static_cast<ConstNodeReference>(static_cast<ConstNodeHolderReference>(value)));
      return p;
   }
};

template<class T, class TNodeTraits, LinkModeType LinkMode, class Tag, unsigned int Type>
struct Bhtraits
   : public BhtraitsBase<T, typename TNodeTraits::node_ptr, Tag, Type>
{
   static const LinkModeType LINK_MODE = LinkMode;
   typedef TNodeTraits NodeTraits;
};


template<class T, class Hook, Hook T::* P>
struct Mhtraits
{
   public:
   typedef Hook                                                      HookType;
   typedef typename HookType::hooktags::NodeTraits                 NodeTraits;
   typedef typename NodeTraits::Node                                Node;
   typedef T                                                         ValueType;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<T>::type                               Pointer;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const T>::type                         ConstPointer;
   typedef T &                                                       Reference;
   typedef const T &                                                 ConstReference;
   typedef Node&                                                     NodeReference;
   typedef const Node &                                              ConstNodeReference;
   typedef HookType&                                                HookReference;
   typedef const HookType &                                         ConstHookReference;

   static const LinkModeType LINK_MODE = Hook::hooktags::LINK_MODE;

   inline static NodePtr to_node_ptr(Reference value)
   {
      return PointerTraits<NodePtr>::pointer_to
         (static_cast<NodeReference>(static_cast<HookReference>(value.*P)));
   }

   inline static ConstNodePtr to_node_ptr(ConstReference value)
   {
      return PointerTraits<ConstNodePtr>::pointer_to
         (static_cast<ConstNodeReference>(static_cast<ConstHookReference>(value.*P)));
   }

   inline static Pointer to_value_ptr(NodePtr n)
   {
      return PointerTraits<Pointer>::pointer_to
         (*detail::parent_from_member<T, Hook>
            (static_cast<Hook*>(boost::movelib::to_raw_pointer(n)), P));
   }

   inline static ConstPointer to_value_ptr(ConstNodePtr n)
   {
      return PointerTraits<ConstPointer>::pointer_to
         (*detail::parent_from_member<T, Hook>
            (static_cast<const Hook*>(boost::movelib::to_raw_pointer(n)), P));
   }
};


template<class Functor>
struct Fhtraits
{
   public:
   typedef typename Functor::HookType                               HookType;
   typedef typename Functor::HookPtr                                HookPtr;
   typedef typename Functor::ConstHookPtr                          ConstHookPtr;
   typedef typename HookType::hooktags::NodeTraits                 NodeTraits;
   typedef typename NodeTraits::Node                                Node;
   typedef typename Functor::ValueType                              ValueType;
   typedef typename NodeTraits::NodePtr                            NodePtr;
   typedef typename NodeTraits::ConstNodePtr                      ConstNodePtr;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<ValueType>::type                      Pointer;
   typedef typename PointerTraits<NodePtr>::
      template rebind_pointer<const ValueType>::type                ConstPointer;
   typedef ValueType &                                              Reference;
   typedef const ValueType &                                        ConstReference;
   static const LinkModeType LINK_MODE = HookType::hooktags::LINK_MODE;

   inline static NodePtr to_node_ptr(Reference value)
   {  return static_cast<Node*>(boost::movelib::to_raw_pointer(Functor::to_hook_ptr(value)));  }

   inline static ConstNodePtr to_node_ptr(ConstReference value)
   {  return static_cast<const Node*>(boost::movelib::to_raw_pointer(Functor::to_hook_ptr(value)));  }

   inline static Pointer to_value_ptr(NodePtr n)
   {  return Functor::to_value_ptr(to_hook_ptr(n));  }

   inline static ConstPointer to_value_ptr(ConstNodePtr n)
   {  return Functor::to_value_ptr(to_hook_ptr(n));  }

   private:
   inline static HookPtr to_hook_ptr(NodePtr n)
   {  return HookPtr(&*static_cast<HookType*>(&*n));  }

   inline static ConstHookPtr to_hook_ptr(ConstNodePtr n)
   {  return ConstHookPtr(&*static_cast<const HookType*>(&*n));  }
};


} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_DETAIL_HOOK_TRAITS_HPP
