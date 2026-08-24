//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Pablo Halpern 2009. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2011-2014. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/intrusive for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_POINTER_TRAITS_HPP
#define BOOST_INTRUSIVE_POINTER_TRAITS_HPP

#include <boost/intrusive/detail/config_begin.hpp>
#include <boost/intrusive/detail/workaround.hpp>
#include <boost/intrusive/pointer_rebind.hpp>
#include <boost/move/detail/pointer_element.hpp>
#include <boost/intrusive/detail/mpl.hpp>
#include <cstddef>

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

namespace boost {
namespace intrusive {
namespace detail {

#if !defined(BOOST_MSVC) || (BOOST_MSVC > 1310)
BOOST_INTRUSIVE_HAS_STATIC_MEMBER_FUNC_SIGNATURE(HasMemberFunctionCallableWithPointerTo, pointer_to)
BOOST_INTRUSIVE_HAS_STATIC_MEMBER_FUNC_SIGNATURE(HasMemberFunctionCallableWithDynamicCastFrom, dynamic_cast_from)
BOOST_INTRUSIVE_HAS_STATIC_MEMBER_FUNC_SIGNATURE(HasMemberFunctionCallableWithStaticCastFrom, static_cast_from)
BOOST_INTRUSIVE_HAS_STATIC_MEMBER_FUNC_SIGNATURE(HasMemberFunctionCallableWithConstCastFrom, const_cast_from)
#else
BOOST_INTRUSIVE_HAS_MEMBER_FUNC_CALLED_IGNORE_SIGNATURE(has_member_function_callable_with_pointer_to, pointer_to)
BOOST_INTRUSIVE_HAS_MEMBER_FUNC_CALLED_IGNORE_SIGNATURE(has_member_function_callable_with_dynamic_cast_from, dynamic_cast_from)
BOOST_INTRUSIVE_HAS_MEMBER_FUNC_CALLED_IGNORE_SIGNATURE(has_member_function_callable_with_static_cast_from, static_cast_from)
BOOST_INTRUSIVE_HAS_MEMBER_FUNC_CALLED_IGNORE_SIGNATURE(has_member_function_callable_with_const_cast_from, const_cast_from)
#endif

BOOST_INTRUSIVE_INSTANTIATE_EVAL_DEFAULT_TYPE_TMPLT(ElementType)
BOOST_INTRUSIVE_INSTANTIATE_DEFAULT_TYPE_TMPLT(DifferenceType)
BOOST_INTRUSIVE_INSTANTIATE_DEFAULT_TYPE_TMPLT(SizeType)
BOOST_INTRUSIVE_INSTANTIATE_DEFAULT_TYPE_TMPLT(Reference)
BOOST_INTRUSIVE_INSTANTIATE_DEFAULT_TYPE_TMPLT(ValueTraitsPtr)

}  //namespace detail {


//! pointer_traits is the implementation of C++11 std::pointer_traits class with some
//! extensions like castings.
//!
//! pointer_traits supplies a uniform interface to certain attributes of pointer-like types.
//!
//! <b>Note</b>: When defining a custom family of pointers or references to be used with BI
//! library, make sure the public static conversion functions accessed through
//! the `pointer_traits` interface (`*_cast_from` and `pointer_to`) can
//! properly convert between const and nonconst referred member types
//! <b>without the use of implicit constructor calls</b>. It is suggested these
//! conversions be implemented as function templates, where the template
//! argument is the type of the object being converted from.
template <typename Ptr>
struct PointerTraits
{
   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
      //!The pointer type
      //!queried by this pointer_traits instantiation
      typedef Ptr             pointer;

      //!Ptr::element_type if such a type exists; otherwise, T if Ptr is a class
      //!template instantiation of the form SomePointer<T, Args>, where Args is zero or
      //!more type arguments ; otherwise , the specialization is ill-formed.
      typedef unspecified_type element_type;

      //!Ptr::difference_type if such a type exists; otherwise,
      //!std::ptrdiff_t.
      typedef unspecified_type difference_type;

      //!Ptr::rebind<U> if such a type exists; otherwise, SomePointer<U, Args> if Ptr is
      //!a class template instantiation of the form SomePointer<T, Args>, where Args is zero or
      //!more type arguments ; otherwise, the instantiation of rebind is ill-formed.
      //!
      //!For portable code for C++03 and C++11, <pre>typename rebind_pointer<U>::type</pre>
      //!shall be used instead of rebind<U> to obtain a pointer to U.
      template <class U> using rebind = unspecified;

      //!Ptr::reference if such a type exists (non-standard extension); otherwise, element_type &
      //!
      typedef unspecified_type reference;
   #else
      typedef Ptr                                                             Pointer;
      //
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_EVAL_DEFAULT
         ( boost::intrusive::detail::, Ptr, ElementType
         , boost::movelib::detail::first_param<Ptr>)                          ElementType;
      //
      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT
         (boost::intrusive::detail::, Ptr, DifferenceType, std::ptrdiff_t)   DifferenceType;

      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT
         ( boost::intrusive::detail::, Ptr, SizeType
         , typename boost::move_detail::
               make_unsigned<DifferenceType>::type)                          SizeType;

      typedef BOOST_INTRUSIVE_OBTAIN_TYPE_WITH_DEFAULT
         ( boost::intrusive::detail::, Ptr, Reference
         , typename boost::intrusive::detail::unvoid_ref<ElementType>::type) Reference;
      //
      template <class U> struct RebindPointer
      {
         typedef typename boost::intrusive::PointerRebind<Ptr, U>::Type  Type;
      };

      #if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
         template <class U> using rebind = typename boost::intrusive::PointerRebind<Ptr, U>::type;
      #endif
   #endif   //#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)

   //! <b>Remark</b>: If element_type is (possibly cv-qualified) void, r type is unspecified; otherwise,
   //!   it is element_type &.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling Ptr::pointer_to(reference).
   //!   Non-standard extension: If such function does not exist, returns pointer(addressof(r));
   //!
   //! <b>Note</b>: For non-conforming compilers only the existence of a member function called
   //!   <code>pointer_to</code> is checked.
   BOOST_INTRUSIVE_FORCEINLINE static Pointer pointer_to(Reference r) BOOST_NOEXCEPT
   {
      //Non-standard extension, it does not require Ptr::pointer_to. If not present
      //tries to converts &r to pointer.
      const bool value = boost::intrusive::detail::
         HasMemberFunctionCallableWithPointerTo
            <Ptr, Ptr (*)(Reference)>::value;
      boost::intrusive::detail::bool_<value> flag;
      return PointerTraits::priv_pointer_to(flag, r);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling the static template function
   //!   Ptr::static_cast_from(UPpr/const UPpr &).
   //!   If such function does not exist, returns pointer_to(static_cast<element_type&>(*uptr))
   //!
   //! <b>Note</b>: For non-conforming compilers only the existence of a member function called
   //!   <code>static_cast_from</code> is checked.
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer static_cast_from(const UPtr &uptr) BOOST_NOEXCEPT
   {
      typedef const UPtr &RefArg;
      const bool value = boost::intrusive::detail::
         HasMemberFunctionCallableWithStaticCastFrom
            <Pointer, Pointer(*)(RefArg)>::value
         || boost::intrusive::detail::
               HasMemberFunctionCallableWithStaticCastFrom
                  <Pointer, Pointer(*)(UPtr)>::value;
      return PointerTraits::priv_static_cast_from(boost::intrusive::detail::bool_<value>(), uptr);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling the static template function
   //!   Ptr::const_cast_from<UPtr>(UPpr/const UPpr &).
   //!   If such function does not exist, returns pointer_to(const_cast<element_type&>(*uptr))
   //!
   //! <b>Note</b>: For non-conforming compilers only the existence of a member function called
   //!   <code>const_cast_from</code> is checked.
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer const_cast_from(const UPtr &uptr) BOOST_NOEXCEPT
   {
      typedef const UPtr &RefArg;
      const bool value = boost::intrusive::detail::
         HasMemberFunctionCallableWithConstCastFrom
            <Pointer, Pointer(*)(RefArg)>::value
         || boost::intrusive::detail::
               HasMemberFunctionCallableWithConstCastFrom
                  <Pointer, Pointer(*)(UPtr)>::value;
      return PointerTraits::priv_const_cast_from(boost::intrusive::detail::bool_<value>(), uptr);
   }

   //! <b>Remark</b>: Non-standard extension.
   //!
   //! <b>Returns</b>: A dereferenceable pointer to r obtained by calling the static template function
   //!   Ptr::dynamic_cast_from<UPtr>(UPpr/const UPpr &).
   //!   If such function does not exist, returns pointer_to(*dynamic_cast<element_type*>(&*uptr))
   //!
   //! <b>Note</b>: For non-conforming compilers only the existence of a member function called
   //!   <code>dynamic_cast_from</code> is checked.
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer dynamic_cast_from(const UPtr &uptr) BOOST_NOEXCEPT
   {
      typedef const UPtr &RefArg;
      const bool value = boost::intrusive::detail::
         HasMemberFunctionCallableWithDynamicCastFrom
            <Pointer, Pointer(*)(RefArg)>::value
         || boost::intrusive::detail::
               HasMemberFunctionCallableWithDynamicCastFrom
                  <Pointer, Pointer(*)(UPtr)>::value;
      return PointerTraits::priv_dynamic_cast_from(boost::intrusive::detail::bool_<value>(), uptr);
   }

   ///@cond
   private:
   //priv_to_raw_pointer
   template <class T>
   BOOST_INTRUSIVE_FORCEINLINE static T* to_raw_pointer(T* p) BOOST_NOEXCEPT
   {  return p; }

   template <class Pointer>
   BOOST_INTRUSIVE_FORCEINLINE static typename PointerTraits<Pointer>::element_type*
      to_raw_pointer(const Pointer &p) BOOST_NOEXCEPT
   {  return PointerTraits::to_raw_pointer(p.operator->());  }

   //priv_pointer_to
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_pointer_to(boost::intrusive::detail::true_, Reference r) BOOST_NOEXCEPT
   { return Ptr::pointer_to(r); }

   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_pointer_to(boost::intrusive::detail::false_, Reference r) BOOST_NOEXCEPT
   { return Pointer(boost::intrusive::detail::addressof(r)); }

   //priv_static_cast_from
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_static_cast_from(boost::intrusive::detail::true_, const UPtr &uptr) BOOST_NOEXCEPT
   { return Ptr::static_cast_from(uptr); }

   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_static_cast_from(boost::intrusive::detail::false_, const UPtr &uptr) BOOST_NOEXCEPT
   {  return uptr ? pointer_to(*static_cast<ElementType*>(to_raw_pointer(uptr))) : Pointer();  }

   //priv_const_cast_from
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_const_cast_from(boost::intrusive::detail::true_, const UPtr &uptr) BOOST_NOEXCEPT
   { return Ptr::const_cast_from(uptr); }

   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_const_cast_from(boost::intrusive::detail::false_, const UPtr &uptr) BOOST_NOEXCEPT
   {  return uptr ? pointer_to(const_cast<ElementType&>(*uptr)) : Pointer();  }

   //priv_dynamic_cast_from
   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_dynamic_cast_from(boost::intrusive::detail::true_, const UPtr &uptr) BOOST_NOEXCEPT
   { return Ptr::dynamic_cast_from(uptr); }

   template<class UPtr>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer priv_dynamic_cast_from(boost::intrusive::detail::false_, const UPtr &uptr) BOOST_NOEXCEPT
   {  return uptr ? pointer_to(dynamic_cast<ElementType&>(*uptr)) : Pointer();  }
   ///@endcond
};

///@cond

// Remove cv qualification from Ptr parameter to pointer_traits:
template <typename Ptr>
struct PointerTraits<const Ptr> : PointerTraits<Ptr> {};
template <typename Ptr>
struct PointerTraits<volatile Ptr> : PointerTraits<Ptr> { };
template <typename Ptr>
struct PointerTraits<const volatile Ptr> : PointerTraits<Ptr> { };
// Remove reference from Ptr parameter to pointer_traits:
template <typename Ptr>
struct PointerTraits<Ptr&> : PointerTraits<Ptr> { };

///@endcond

//! Specialization of pointer_traits for raw pointers
//!
template <typename T>
struct PointerTraits<T*>
{
   typedef T               ElementType;
   typedef T*              Pointer;
   typedef std::ptrdiff_t  DifferenceType;
   typedef std::size_t     SizeType;

   #ifdef BOOST_INTRUSIVE_DOXYGEN_INVOKED
      typedef T &          reference;
      //!typedef for <pre>U *</pre>
      //!
      //!For portable code for C++03 and C++11, <pre>typename rebind_pointer<U>::type</pre>
      //!shall be used instead of rebind<U> to obtain a pointer to U.
      template <class U> using rebind = U*;
   #else
      typedef typename boost::intrusive::detail::unvoid_ref<ElementType>::type Reference;
      #if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
         template <class U> using rebind = U*;
      #endif
   #endif

   template <class U> struct RebindPointer
   {  typedef U* Type;  };

   //! <b>Returns</b>: addressof(r)
   //!
   BOOST_INTRUSIVE_FORCEINLINE static Pointer pointer_to(Reference r) BOOST_NOEXCEPT
   { return boost::intrusive::detail::addressof(r); }

   //! <b>Returns</b>: static_cast<pointer>(uptr)
   //!
   template<class U>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer static_cast_from(U *uptr) BOOST_NOEXCEPT
   {  return static_cast<Pointer>(uptr);  }

   //! <b>Returns</b>: const_cast<pointer>(uptr)
   //!
   template<class U>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer const_cast_from(U *uptr) BOOST_NOEXCEPT
   {  return const_cast<Pointer>(uptr);  }

   //! <b>Returns</b>: dynamic_cast<pointer>(uptr)
   //!
   template<class U>
   BOOST_INTRUSIVE_FORCEINLINE static Pointer dynamic_cast_from(U *uptr) BOOST_NOEXCEPT
   {  return dynamic_cast<Pointer>(uptr);  }
};

}  //namespace container {
}  //namespace boost {

#include <boost/intrusive/detail/config_end.hpp>

#endif // ! defined(BOOST_INTRUSIVE_POINTER_TRAITS_HPP)
