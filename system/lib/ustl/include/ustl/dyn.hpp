/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_DYN_HPP
#define USTL_DYN_HPP 1

#include <memory>
#include <type_traits>
#include <utility>

/// Dynamic dispatch without a base class.
///
/// An interface is a list of methods, `Dyn<Method<...>, ...>`. Any type that
/// answers every method models it, whatever it derives from and whether or not
/// it knows the interface exists, and a `Ref<Dyn<...>>` refers to such an
/// object and calls its methods through a table of function pointers built for
/// that type at compile time. It is the call a virtual function makes, with
/// the table kept beside the reference instead of inside the object: the
/// object needs no vptr, no virtual destructor and no common base, and the
/// interface can be written for a type that already exists.
///
/// Nothing here allocates, throws, or asks for run-time type information, so it
/// builds under the kernel's -ffreestanding -fno-exceptions -fno-rtti.
///
///     USTL_DYN_MEMBER(Enqueue, enqueue_thread);
///     USTL_DYN_MEMBER_OR(Yield, yield_thread, ustl::dyn::Ignore{});
///
///     using Schedule = ustl::Dyn<ustl::Method<Enqueue, auto(Queue &, Thread *) -> Status>,
///                                ustl::Method<Yield, auto(Queue &, Thread *) -> void>>;
///
///     ustl::Ref<Schedule> schedule = round_robin;
///     schedule.call<Enqueue>(queue, thread);

namespace ustl {

namespace dyn {
    /// The fallback of an optional method that has nothing to do.
    struct Ignore {
        template <typename... Args>
        constexpr auto operator()(Args &&.../*args*/) const noexcept -> void {}
    };

    /// The fallback of an optional method that answers |Value| when the type
    /// has no function of its own.
    template <auto Value>
    struct Returns {
        template <typename... Args>
        constexpr auto operator()(Args &&.../*args*/) const noexcept -> decltype(Value) {
            return Value;
        }
    };
} // namespace dyn

namespace detail {
    /// The function pointers of one table, in the order of the interface's
    /// methods. A recursive struct, not a tuple, so that it is a literal type
    /// without a library header and a table can be a constant.
    template <typename... Fns>
    struct FnList {};

    template <typename Head, typename... Tail>
    struct FnList<Head, Tail...> {
        constexpr FnList(Head const head_, Tail const... tail_) noexcept
            : head(head_), tail(tail_...) {}

        Head head;
        FnList<Tail...> tail;
    };

    template <usize Index, typename Head, typename... Tail>
    constexpr auto fn_at(FnList<Head, Tail...> const &list) noexcept -> decltype(auto) {
        if constexpr (Index == 0) {
            return list.head;
        } else {
            return fn_at<Index - 1>(list.tail);
        }
    }

    template <typename... Types>
    inline constexpr bool ALL_DISTINCT = true;

    template <typename Head, typename... Tail>
    inline constexpr bool ALL_DISTINCT<Head, Tail...> =
        (!std::is_same_v<Head, Tail> && ...) && ALL_DISTINCT<Tail...>;

    /// The method among |Methods| whose dispatch is |Dispatch|, or void.
    template <typename Dispatch, typename... Methods>
    struct FindMethod {
        using Type = void;
    };

    template <typename Dispatch, typename Head, typename... Tail>
    struct FindMethod<Dispatch, Head, Tail...> {
        using Type = std::conditional_t<std::is_same_v<Dispatch, typename Head::Dispatcher>, Head,
                                        typename FindMethod<Dispatch, Tail...>::Type>;
    };
} // namespace detail

/// One method of an interface: the dispatch that says how an object answers
/// it, and the signature the erased call has. |Dispatch| is a type with a
/// static call operator that takes the object first, which USTL_DYN_MEMBER
/// writes for the usual case, and |Signature| is `auto(Args...) -> R`.
template <typename Dispatch, typename Signature>
struct Method;

template <typename Dispatch, typename R, typename... Args>
struct Method<Dispatch, R(Args...)> {
    using Dispatcher = Dispatch;
    using Result = R;

    /// What the table holds for this method: the function that calls the
    /// object's answer, the object itself erased to a pointer.
    using Thunk = R (*)(void *, Args...);

    /// Whether an object of type |T| answers this method.
    template <typename T>
    static constexpr bool ANSWERED_BY = std::is_invocable_r_v<R, Dispatch, T &, Args...>;

    template <typename T>
    static auto thunk(void *const object, Args... args) -> R {
        return Dispatch{}(*static_cast<T *>(object), std::forward<Args>(args)...);
    }

    /// The thunk of the empty reference: calling nothing stops at once, not
    /// somewhere past a null pointer.
    [[noreturn]]
    static auto thunk_empty(void * /*object*/, Args... /*args*/) -> R {
        __builtin_trap();
    }
};

/// An interface: a list of methods. It has no objects of its own; it names what
/// a type must answer to be referred to through it, and builds the table of
/// function pointers for each such type.
template <typename... Methods>
class Dyn final {
    static_assert(detail::ALL_DISTINCT<typename Methods::Dispatcher...>,
                  "two methods of one interface have the same dispatch, so a call cannot tell "
                  "which it means");

  public:
    using Table = detail::FnList<typename Methods::Thunk...>;

    /// Whether |T| answers every method. A const object is refused: the
    /// reference holds a mutable pointer.
    template <typename T>
    static constexpr bool MODELED_BY =
        !std::is_const_v<T> && (Methods::template ANSWERED_BY<T> && ...);

    /// Whether the interface has a method with this dispatch.
    template <typename Dispatch>
    static constexpr bool HAS = (std::is_same_v<Dispatch, typename Methods::Dispatcher> || ...);

    /// The method with this dispatch, void if there is none.
    template <typename Dispatch>
    using MethodOf = typename detail::FindMethod<Dispatch, Methods...>::Type;

    /// The position of the method with this dispatch in the table.
    template <typename Dispatch>
    static consteval auto index_of() -> usize {
        constexpr bool SAME[] = {std::is_same_v<Dispatch, typename Methods::Dispatcher>..., false};
        auto index = usize{0};
        while (!SAME[index]) {
            index += 1;
        }
        return index;
    }

    /// The table of |T|, one per type, in read-only data.
    template <typename T>
    static constexpr Table TABLE_FOR = Table{&Methods::template thunk<T>...};

    /// The table of the empty reference, every entry trapping. Its address is
    /// what says a reference is empty.
    static constexpr Table EMPTY = Table{&Methods::thunk_empty...};
};

/// A reference to an object that models the interface |Interface|: a pointer
/// to the object and a pointer to the table for its type, two words, trivially
/// copyable. It owns nothing and does not extend the object's lifetime; the
/// object has to be there, at the same address, for as long as a copy of the
/// reference is called.
template <typename Interface>
class Ref;

template <typename... Methods>
class Ref<Dyn<Methods...>> final {
    using Interface = Dyn<Methods...>;

  public:
    /// The empty reference. Calling it traps.
    constexpr Ref() noexcept : m_object(nullptr), m_table(&Interface::EMPTY) {}

    template <typename T>
        requires(Interface::template MODELED_BY<T> && !std::is_same_v<T, Ref>)
    constexpr Ref(T &object) noexcept
        : m_object(std::addressof(object)), m_table(&Interface::template TABLE_FOR<T>) {}

    [[nodiscard]]
    explicit constexpr operator bool() const noexcept {
        return m_table != &Interface::EMPTY;
    }

    /// Calls the method whose dispatch is |Dispatch| on the object. The result
    /// is the method's declared one, a reference included. A call that names
    /// no method of the interface, or whose arguments do not convert to the
    /// method's declared ones, does not exist: it is rejected by the
    /// constraint, not by an error inside.
    template <typename Dispatch, typename... CallArgs>
        requires(Interface::template HAS<Dispatch> &&
                 std::is_invocable_v<typename Interface::template MethodOf<Dispatch>::Thunk, void *,
                                     CallArgs...>)
    auto call(CallArgs &&...args) const -> typename Interface::template MethodOf<Dispatch>::Result {
        return detail::fn_at<Interface::template index_of<Dispatch>()>(*m_table)(
            m_object, std::forward<CallArgs>(args)...);
    }

  private:
    void *m_object;
    typename Interface::Table const *m_table;
};

} // namespace ustl

/// Declares the dispatch |Name| for the method an object answers with its
/// member function |member|: `Name{}(object, args...)` is
/// `object.member(args...)`, and an object without such a member does not
/// model an interface that has the method.
///
/// A macro, because a dispatch has to name a member, which only a macro or a
/// lambda per method can do, and the lambda's closure type could not be a
/// template argument without a constant to hold it (the user's call,
/// 2026-11-03).
#define USTL_DYN_MEMBER(Name, member)                                                              \
    struct Name {                                                                                  \
        template <typename Self, typename... Args>                                                 \
        static constexpr auto                                                                      \
        operator()(Self &self,                                                                     \
                   Args &&...args) noexcept(noexcept(self.member(std::forward<Args>(args)...)))    \
            -> decltype(self.member(std::forward<Args>(args)...)) {                                \
            return self.member(std::forward<Args>(args)...);                                       \
        }                                                                                          \
    }

/// Like USTL_DYN_MEMBER for an optional method: an object that has no member
/// |member| answers with |fallback|, a function object called with the
/// method's arguments, such as `ustl::dyn::Ignore{}` or
/// `ustl::dyn::Returns<Status::NotSupported>{}`. Every object models the
/// method, and the default is written once, where the method is declared.
#define USTL_DYN_MEMBER_OR(Name, member, fallback)                                                 \
    struct Name {                                                                                  \
        template <typename Self, typename... Args>                                                 \
        static constexpr auto operator()([[maybe_unused]] Self &self,                              \
                                         [[maybe_unused]] Args &&...args) -> decltype(auto) {      \
            if constexpr (requires { self.member(std::forward<Args>(args)...); }) {                \
                return self.member(std::forward<Args>(args)...);                                   \
            } else {                                                                               \
                return (fallback)(std::forward<Args>(args)...);                                    \
            }                                                                                      \
        }                                                                                          \
    }

#endif // #ifndef USTL_DYN_HPP
