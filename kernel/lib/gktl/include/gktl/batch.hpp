/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_FOR_EACH_MEMBER_HPP
#define GKTL_FOR_EACH_MEMBER_HPP 1

#include <ustl/traits/integral_constant.hpp>
#include <ustl/util/pair.hpp>
#include <ustl/util/tuple.hpp>

namespace gktl {
namespace action {
    template <auto... Params>
    struct CompileParamsPacket {};

    template <typename... Params>
    struct RuntimeParamsPacket {
        RuntimeParamsPacket(Params &&...params) : m_value(ustl::forward<Params>(params)...) {}

        ~RuntimeParamsPacket() = default;

        ustl::Tuple<Params...> m_value;
    };

    template <auto Value>
    struct Param {};

    template <typename... Params>
    struct MixedParamsPacket {
        template <typename... Args>
        MixedParamsPacket(Args...);

        ~MixedParamsPacket() = default;
    };

    template <typename F, typename ParamsPacket>
    struct Invoker;

    template <typename F, auto... Params>
    struct Invoker<F, CompileParamsPacket<Params...>> {
        static auto invoke(F f) -> bool {
            return f(Params...);
        }
    };

    template <typename F, typename... Params>
    struct Invoker<F, RuntimeParamsPacket<Params...>> {
        static auto invoke(F f, RuntimeParamsPacket<Params...> const &params) -> bool {
            return f(params);
        }
    };

    template <typename... StaticParamsPackets, typename Predicate, typename OnCompelete,
              typename OnError, typename... DynamaicParamsPackets>
    auto batch(Predicate predicate, OnCompelete on_compelete, OnError on_error,
               DynamaicParamsPackets &&...dyn_packets) -> bool {
        if ((Invoker<Predicate, StaticParamsPackets>::invoke(predicate) && ...)) {
            if ((Invoker<Predicate, DynamaicParamsPackets>::invoke(predicate, dyn_packets) &&
                 ...)) {
                (Invoker<OnCompelete, StaticParamsPackets>::invoke(on_compelete), ...);
                (Invoker<OnCompelete, DynamaicParamsPackets>::invoke(on_compelete, dyn_packets),
                 ...);
                return true;
            }
        }

        return false;
    }

} // namespace action

template <typename T, typename F, typename Member>
auto for_each_one_member(T &owner, F &&f, Member T::*member) -> void {
    f(owner->*member);
}

template <int Index = 0, typename T, typename F, typename FirstMember, typename... Members>
auto for_each_members(T &owner, F &&f, FirstMember T::*member, Members T::*...members) -> void {
    f(Index, owner->*member);

    if (sizeof...(Members) > 0) {
        for_each_members<Index + 1>(owner, f, members...);
    }
}

template <int Index = 0, typename T, typename F, typename FirstMember, typename... Members>
auto for_each_members(F &&f, FirstMember T::*member, Members T::*...members) -> void {
    f(member, ustl::traits::IntegralConstant<int, Index>());

    if (sizeof...(Members) > 0) {
        for_each_members<T, Index + 1>(f, members...);
    }
}

template <int Index = 0, typename T, typename F, typename Member, typename Param,
          typename... Members, typename... Params>
auto for_each_members_parameterized(F &&f, ustl::Pair<Member T::*, Param> member_packed,
                                    ustl::Pair<Members T::*, Params>... members_packed) -> void {
    auto [member, param] = member_packed;
    f(member, param, ustl::traits::IntegralConstant<int, Index>());

    if (sizeof...(Members) > 0) {
        for_each_members_parameterized<Index + 1>(f, members_packed...);
    }
}
} // namespace gktl

#endif // #ifndef GKTL_FOR_EACH_MEMBER_HPP