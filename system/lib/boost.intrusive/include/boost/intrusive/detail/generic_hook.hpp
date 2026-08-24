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

#ifndef BOOST_INTRUSIVE_GENERIC_HOOK_HPP
#define BOOST_INTRUSIVE_GENERIC_HOOK_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#include <boost/intrusive/pointer_traits.hpp>
#include <boost/intrusive/link_mode.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <boost/intrusive/detail/assert.hpp>
#include <boost/intrusive/detail/node_holder.hpp>
#include <boost/intrusive/detail/algo_type.hpp>

namespace boost {
namespace intrusive {

/// @cond

namespace detail {

template <LinkModeType LinkMode>
struct LinkDispatch
{};

template<class Hook>
inline void destructor_impl(Hook &hook, detail::LinkDispatch<SafeLink>)
{  //If this assertion raises, you might have destroyed an object
   //while it was still inserted in a container that is alive.
   //If so, remove the object from the container before destroying it.
   (void)hook; BOOST_INTRUSIVE_SAFE_HOOK_DESTRUCTOR_ASSERT(!hook.is_linked());
}

template<class Hook>
inline void destructor_impl(Hook &hook, detail::LinkDispatch<AutoUnlink>)
{  hook.unlink();  }

template<class Hook>
inline void destructor_impl(Hook &, detail::LinkDispatch<NormalLink>)
{}

}  //namespace detail {

enum BaseHookType
{  NoBaseHookId
,  ListBaseHookId
,  SlistBaseHookId
,  RbTreeBaseHookId
,  HashBaseHookId
,  AvlTreeBaseHookId
,  BsTreeBaseHookId
,  TreapTreeBaseHookId
,  AnyBaseHookId
};


template <class HookTags, unsigned int>
struct HookTagsDefiner{};

template <class HookTags>
struct HookTagsDefiner<HookTags, ListBaseHookId>
{  typedef HookTags DefaultListHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, SlistBaseHookId>
{  typedef HookTags DefaultSlistHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, RbTreeBaseHookId>
{  typedef HookTags DefaultRbtreeHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, HashBaseHookId>
{  typedef HookTags DefaultHashtableHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, AvlTreeBaseHookId>
{  typedef HookTags DefaultAvltreeHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, BsTreeBaseHookId>
{  typedef HookTags DefaultBstreeHook;  };

template <class HookTags>
struct HookTagsDefiner<HookTags, AnyBaseHookId>
{  typedef HookTags DefaultAnyHook;  };

template
   < class TNodeTraits
   , class TTag
   , LinkModeType LinkMode
   , BaseHookType BaseHookType
   >
struct HooktagsImpl
{
   static const LinkModeType LINK_MODE = LinkMode;
   typedef TTag Tag;
   typedef TNodeTraits NodeTraits;
   static const bool IS_BASE_HOOK = !detail::is_same<TTag, member_tag>::value;
   static const bool SAFEMODE_OR_AUTOUNLINK = IsSafeAutounlink<LINK_MODE>::VALUE;
   static const unsigned int TYPE = BaseHookType;
};

/// @endcond

template
   < boost::intrusive::AlgoTypes Algo
   , class NodeTraits
   , class Tag
   , LinkModeType LinkMode
   , BaseHookType BaseHookType
   >
class GenericHook
   /// @cond
   //If the hook is a base hook, derive generic hook from node_holder
   //so that a unique base class is created to convert from the node
   //to the type. This mechanism will be used by bhtraits.
   //
   //If the hook is a member hook, generic hook will directly derive
   //from the hook.
   : public detail::if_c
      < detail::is_same<Tag, member_tag>::value
      , typename NodeTraits::node
      , NodeHolder<typename NodeTraits::node, Tag, BaseHookType>
      >::type
   //If this is the a default-tagged base hook derive from a class that
   //will define an special internal typedef. Containers will be able to detect this
   //special typedef and obtain generic_hook's internal types in order to deduce
   //value_traits for this hook.
   , public HookTagsDefiner
      < GenericHook<Algo, NodeTraits, Tag, LinkMode, BaseHookType>
      , detail::is_same<Tag, dft_tag>::value ? BaseHookType : NoBaseHookId>
   /// @endcond
{
   /// @cond
   typedef typename get_algo<Algo, NodeTraits>::type  NodeAlgorithms;
   typedef typename NodeAlgorithms::Node             Node;
   typedef typename NodeAlgorithms::NodePtr         NodePtr;
   typedef typename NodeAlgorithms::ConstNodePtr   ConstNodePtr;

   public:

   typedef HooktagsImpl
      < NodeTraits
      , Tag, LinkMode, BaseHookType>                  Hooktags;

   inline NodePtr this_ptr() BOOST_NOEXCEPT
   {  return PointerTraits<NodePtr>::pointer_to(static_cast<Node&>(*this)); }

   inline ConstNodePtr this_ptr() const BOOST_NOEXCEPT
   {  return PointerTraits<ConstNodePtr>::pointer_to(static_cast<const Node&>(*this)); }

   public:
   /// @endcond

   inline GenericHook() BOOST_NOEXCEPT
   #if defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
      requires (LinkMode != NormalLink)
   #endif
   {
      BOOST_IF_CONSTEXPR(Hooktags::safemode_or_autounlink){
         NodeAlgorithms::init(this->this_ptr());
      }
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) && defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
   //Default destructor for normal links (allows conditional triviality)
   GenericHook() requires (LinkMode == NormalLink) = default;
   #endif

   inline GenericHook(const GenericHook& ) BOOST_NOEXCEPT
   {
      BOOST_IF_CONSTEXPR(Hooktags::safemode_or_autounlink){
         NodeAlgorithms::init(this->this_ptr());
      }
   }

   inline GenericHook& operator=(const GenericHook& ) BOOST_NOEXCEPT
   {  return *this;  }

   inline ~GenericHook()
   #if defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
      requires (LinkMode != NormalLink)
   #endif
   {
      destructor_impl
         (*this, detail::LinkDispatch<Hooktags::link_mode>());
   }

   #if !defined(BOOST_INTRUSIVE_DOXYGEN_INVOKED) && defined(BOOST_INTRUSIVE_CONCEPTS_BASED_OVERLOADING)
   //Default destructor for normal links (allows conditional triviality)
   ~GenericHook() requires (LinkMode == NormalLink) = default;
   #endif

   inline void swap_nodes(GenericHook &other) BOOST_NOEXCEPT
   {
      NodeAlgorithms::swap_nodes
         (this->this_ptr(), other.this_ptr());
   }

   [[nodiscard]] inline bool is_linked() const BOOST_NOEXCEPT
   {
      //is_linked() can be only used in safe-mode or auto-unlink
      BOOST_INTRUSIVE_STATIC_ASSERT(( Hooktags::safemode_or_autounlink ));
      return !NodeAlgorithms::unique(this->this_ptr());
   }

   inline void unlink() BOOST_NOEXCEPT
   {
      BOOST_INTRUSIVE_STATIC_ASSERT(( (int)Hooktags::link_mode == (int)AutoUnlink ));
      NodePtr n(this->this_ptr());
      if(!NodeAlgorithms::inited(n)){
         NodeAlgorithms::unlink(n);
         NodeAlgorithms::init(n);
      }
   }
};

} //namespace intrusive
} //namespace boost

#endif //BOOST_INTRUSIVE_GENERIC_HOOK_HPP
