/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_DYN_HPP
#define USTL_DYN_HPP 1

#include <array>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <cstddef>

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
        constexpr FnList(Head const first, Tail const... rest) noexcept
            : head(first), tail(rest...) {}

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
        // launder: for an Own the pointer is the address of its storage, in
        // which the object was constructed, and not a pointer to the object.
        return Dispatch{}(*std::launder(static_cast<T *>(object)), std::forward<Args>(args)...);
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
/// The default buffer is three pointers, as ustl::Function's is.
template <typename Interface, usize Size = 3 * sizeof(void *), usize Align = alignof(void *)>
class Own;

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
    template <typename, usize, usize>
    friend class Own;

    constexpr Ref(void *const object, typename Interface::Table const *const table) noexcept
        : m_object(object), m_table(table) {}

    void *m_object;
    typename Interface::Table const *m_table;
};

namespace detail {
    /// What a target of type T must satisfy to live in |Size| bytes aligned to
    /// |Align|, as ustl::Function asks. A concept, so that "does not fit" takes
    /// part in overload resolution and a failed one is reported as the clause
    /// that failed.
    template <typename T, usize Size, usize Align>
    concept FitsInline =
        sizeof(T) <= Size && alignof(T) <= Align && std::is_nothrow_move_constructible_v<T>;
} // namespace detail

/// An object that models the interface, living inside the Own itself: the
/// owning counterpart of Ref, as ustl::Function is the owning callable. Where a
/// Ref refers to an object somewhere else and must not outlive it, an Own
/// holds one, moves it when it is moved and destroys it when it dies.
///
/// It never allocates. The object lives in |Size| bytes aligned to |Align|, and
/// a type that does not fit, is aligned more strictly, or could throw from its
/// move constructor does not construct an Own: it is a compile error at the line
/// that built it, not a trip to the heap. The default holds three pointers.
/// Different sizes are different types, as with any buffer.
///
/// Move-only, like ustl::Function: copying would demand that every held type be
/// copyable. Moving relocates the object into the destination, and the source is
/// left empty. An empty Own is not a null pointer but a table whose methods
/// trap, so a call has no branch and calling nothing stops at once.
template <typename... Methods, usize Size, usize Align>
class Own<Dyn<Methods...>, Size, Align> final {
    using Interface = Dyn<Methods...>;

    template <typename T>
    static constexpr bool HOLDS =
        Interface::template MODELED_BY<T> && detail::FitsInline<T, Size, Align>;

  public:
    /// The empty Own. Calling it traps.
    Own() noexcept = default;

    /// Holds a copy of, or the moved value of, |target|.
    template <typename T>
        requires(HOLDS<std::remove_cvref_t<T>> && !std::is_same_v<std::remove_cvref_t<T>, Own> &&
                 std::is_constructible_v<std::remove_cvref_t<T>, T>)
    Own(T &&target) noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<T>, T>)
        : m_ops(&OPS<std::remove_cvref_t<T>>) {
        std::construct_at(static_cast<std::remove_cvref_t<T> *>(storage()),
                          std::forward<T>(target));
    }

    /// Constructs a |T| in place from |args|.
    template <typename T, typename... Args>
        requires(HOLDS<T> && std::is_constructible_v<T, Args...>)
    explicit Own(std::in_place_type_t<T>,
                 Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        : m_ops(&OPS<T>) {
        std::construct_at(static_cast<T *>(storage()), std::forward<Args>(args)...);
    }

    Own(Own &&other) noexcept {
        take(other);
    }

    auto operator=(Own &&other) noexcept -> Own & {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    Own(Own const &) = delete;
    auto operator=(Own const &) -> Own & = delete;

    ~Own() {
        m_ops->destroy(storage());
    }

    /// Destroys the held object, if any, and leaves the Own empty.
    auto reset() noexcept -> void {
        m_ops->destroy(storage());
        m_ops = &EMPTY_OPS;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return m_ops != &EMPTY_OPS;
    }

    /// Calls the method whose dispatch is |Dispatch| on the held object, with
    /// the guarantees of Ref::call(). Not const: the object is this Own's own,
    /// and the method may change it.
    template <typename Dispatch, typename... CallArgs>
        requires(Interface::template HAS<Dispatch> &&
                 std::is_invocable_v<typename Interface::template MethodOf<Dispatch>::Thunk, void *,
                                     CallArgs...>)
    auto call(CallArgs &&...args) -> typename Interface::template MethodOf<Dispatch>::Result {
        return detail::fn_at<Interface::template index_of<Dispatch>()>(*m_ops->methods)(
            storage(), std::forward<CallArgs>(args)...);
    }

    /// A reference to the held object. It is good until this Own is moved from,
    /// reset or destroyed; an empty Own gives an empty Ref.
    [[nodiscard]]
    auto ref() noexcept -> Ref<Interface> {
        return Ref<Interface>{storage(), m_ops->methods};
    }

  private:
    /// What is known about the held type, one table per type: its methods, how to
    /// move it into another buffer and how to destroy it.
    struct Ops {
        typename Interface::Table const *methods;
        auto (*relocate)(void *dst, void *src) noexcept -> void;
        auto (*destroy)(void *target) noexcept -> void;
    };

    /// The object lives in the storage, constructed by std::construct_at;
    /// std::launder is what turns the storage's address back into a pointer to
    /// that object rather than to the bytes under it.
    template <typename T>
    static auto target(void *const storage) noexcept -> T * {
        return std::launder(static_cast<T *>(storage));
    }

    template <typename T>
    static auto relocate_target(void *const dst, void *const src) noexcept -> void {
        std::construct_at(static_cast<T *>(dst), std::move(*target<T>(src)));
        std::destroy_at(target<T>(src));
    }

    template <typename T>
    static auto destroy_target(void *const storage) noexcept -> void {
        std::destroy_at(target<T>(storage));
    }

    static auto relocate_empty(void * /*dst*/, void * /*src*/) noexcept -> void {}

    static auto destroy_empty(void * /*storage*/) noexcept -> void {}

    template <typename T>
    static constexpr Ops OPS = {
        .methods = &Interface::template TABLE_FOR<T>,
        .relocate = &relocate_target<T>,
        .destroy = &destroy_target<T>,
    };

    static constexpr Ops EMPTY_OPS = {
        .methods = &Interface::EMPTY,
        .relocate = &relocate_empty,
        .destroy = &destroy_empty,
    };

    /// Moves |other|'s object into this Own, which must be empty, and leaves
    /// |other| empty.
    auto take(Own &other) noexcept -> void {
        m_ops = other.m_ops;
        m_ops->relocate(storage(), other.storage());
        other.m_ops = &EMPTY_OPS;
    }

    [[nodiscard]]
    auto storage() noexcept -> void * {
        return m_storage.data();
    }

    Ops const *m_ops = &EMPTY_OPS;

    /// Left uninitialised: only a constructed object is ever read from it.
    alignas(Align) std::array<std::byte, Size> m_storage;
};

} // namespace ustl

/// Declares the dispatch |Name| for the method an object answers with its
/// member function |member|: `Name{}(object, args...)` is
/// `object.member(args...)`, and an object without such a member does not
/// model an interface that has the method.
///
/// The dispatch is exempt from the thread-safety analysis, and has to be: a
/// lock a member requires is named by one of its arguments (`TA_REQUIRES(rq.lock())`),
/// and the dispatch forwards those without knowing which is which. A caller
/// that needs the check makes it before the erased call, in a function that
/// has the argument by name.
///
/// A macro, because a dispatch has to name a member, which only a macro or a
/// lambda per method can do, and the lambda's closure type could not be a
/// template argument without a constant to hold it (the user's call,
/// 2026-11-03).
#define USTL_DYN_MEMBER(Name, member)                                                              \
    struct Name {                                                                                  \
        template <typename Self, typename... Args>                                                 \
        [[clang::no_thread_safety_analysis]]                                                       \
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
        [[clang::no_thread_safety_analysis]]                                                       \
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
