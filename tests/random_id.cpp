#include <source_location>
#include <iostream>

#include <cstdint>
#include <type_traits>

template <typename Class, typename Lock, int Index= std::source_location::current().line()>
class LockClass {};

template <typename Class, typename Lock>
struct MakeLockDep {
    template <int N>
    struct reader {
        friend auto flag(reader);
    };

    template <int N>
    struct setter: std::integral_constant<int, N> {
        friend auto flag(reader<N>) {}
    };

    template <int N>
    static consteval auto next() {
        if constexpr( requires { flag(reader<N>{}); } ) {
            return next<N + 1>();
        } else {
            setter<N> setter;
            return LockClass<Class, Lock, N>{};
        }
    }

    using Type = decltype(next<0>());
};

template <typename Class, typename Lock>
using LockDep = typename MakeLockDep<Class, Lock>::Type;

struct SpinLock;

class Foo {
    using Self = Foo;
  public:
  private:
    LockDep<Self, SpinLock> x;
    LockDep<Self, SpinLock> y;
    static_assert(!std::is_same_v<decltype(x), decltype(y)>);
}; // class Foo
