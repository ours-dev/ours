/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_TRAITS_INVOKE_ARGS_HPP
#define USTL_TRAITS_INVOKE_ARGS_HPP 1

#include <ustl/util/type_list.hpp>

namespace ustl::traits {

template <typename Signature>
struct InvokeArgs;

template <typename Result, typename... Parameters>
struct InvokeArgs<Result(Parameters...)> {
    using Args = TypeList<Parameters...>;
};

template <typename Result, typename... Parameters>
struct InvokeArgs<Result (*)(Parameters...)>: InvokeArgs<Result(Parameters...)> {};

template <typename Result, typename Class, typename... Parameters>
struct InvokeArgs<Result (Class::*)(Parameters...)>: InvokeArgs<Result(Parameters...)> {};

template <typename Result, typename Class, typename... Parameters>
struct InvokeArgs<Result (Class::*)(Parameters...) const>: InvokeArgs<Result(Parameters...)> {};

} // namespace ustl::traits

#endif // USTL_TRAITS_INVOKE_ARGS_HPP
