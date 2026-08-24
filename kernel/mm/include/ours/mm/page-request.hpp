/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_PAGE_REQUEST_HPP
#define OURS_MM_PAGE_REQUEST_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/rc.hpp>

namespace ours::mm {
class PageRequest {
    using Self = PageRequest;

  public:
    auto wait(bool suspendable) -> Status;
};

class AnonymousPageRequest {};

class MultiPageRequest {
    using Self = PageRequest;

  public:
    auto wait(bool suspendable) -> Status;

    auto get_anonymous() -> AnonymousPageRequest * {
        return &m_anonymous;
    }

  private:
    AnonymousPageRequest m_anonymous;
};

class PageSource: public ustl::RefCounted<PageSource, ustl::sync::ThreadSafePolicy> {};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PAGE_REQUEST_HPP