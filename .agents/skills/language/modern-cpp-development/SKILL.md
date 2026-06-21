---
name: modern-cpp-development
description: Implements anything in modern c++
---

# Modern C++ Development

## Overview

Build features in `modern c++`(20 and later)

## When to use

Activate (satify any of the following criteria): 

- if the standard of c++ of this project is specified to `modern c++`.
- if the user explicitly requires you to code in `modern c++`.

## Requirements

1. Const-first principle: In all contexts, every definition of a variable, function, function parameter, or template parameter should be qualified with one of __const__, __constexpr__, __constinit__, or __consteval__, unless the object needs to be mutable.

1. Treat virtual functions with caution: For everything else, static polymorphism (CRTP), build‑time configuration, or type erasure are better choices. Take VmAspace as an example: we have architecture‑specific versions (ArchVmAspace for x86, ARM, RISC‑V), but the architecture is fixed at build time. So we don't need a virtual base class; we can just plug in the right ArchVmAspace statically (e.g., as a template member) and avoid unnecessary overhead. Therefore, you should always consider whether virtual functions are necessary before using them.

1. Decline to use macros: Macros are a powerful but dangerous feature of C++. They can lead to code that is difficult to read, debug, and maintain. Instead of using macros, you should prefer to use inline functions, constexpr functions, templates or annotations after c++26.

1. Encapsulating implementation details into an inner class:

1. Combine over inheritance:

1. Using alias over typedef

1. Using `auto` for type inference

1. Always prefer lambda experessions for short, inline functions, especially when they are used as arguments to algorithms or for defining small function objects.

1. Always prefer `enum class` over traditional `enum` for better type safety and scoping.

1. Always use trailing return types for all function signatures, even for functions which return void.

1. Using `[[nodiscard]]` attribute for functions that return a value that should not be ignored.

1. Using `[[maybe_unused]]` attribute for variables, functions, or parameters that may not be used to avoid compiler warnings.


1. Using `nullptr` over `NULL` or `0`

1. Using `override` and `final` for virtual functions

1. Using `final` for classes that are not meant to be inherited

1. Using smart pointers (e.g., `std::unique_ptr`, `std::shared_ptr`) instead of raw pointers for better memory management. But be cautious with `std::shared_ptr` as it can lead to performance issues and memory leaks if not used properly. Prefer `std::unique_ptr` when ownership semantics are clear.

## Workflow

## References

[cppreference](https://cppreference.com): a comprehensive online reference for C++ programming language, including detailed information on C++20 and later features.