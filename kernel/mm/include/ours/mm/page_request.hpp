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

/// A bundle of the typed sub-requests one page fault may need to raise: the
/// caller hands the whole bundle down and each layer picks out the slot it
/// knows how to fill.
class MultiPageRequest {
    using Self = MultiPageRequest;

  public:
    auto wait(bool suspendable) -> Status;

    auto get_anonymous() -> AnonymousPageRequest * {
        return &m_anonymous;
    }

    auto get_read() -> PageRequest * {
        return &m_read;
    }

  private:
    AnonymousPageRequest m_anonymous;
    PageRequest m_read;
};

class PageSource: public ustl::RefCounted<PageSource> {};

} // namespace ours::mm

#endif // #ifndef OURS_MM_PAGE_REQUEST_HPP