/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_PAGE_REQUEST_HPP
#define OURS_MEM_PAGE_REQUEST_HPP 1

#include <ours/types.hpp>
#include <ours/status.hpp>

#include <ustl/rc.hpp>

namespace ours::mem {
    class PageRequest {
        using Self = PageRequest;
      public:
        auto wait(bool suspendable) -> Status;
    };

    class AnonymousPageRequest {

    };

    class MultiPageRequest {
        using Self = PageRequest;
      public:
        auto wait(bool suspendable) -> Status;

        auto get_anonymous() -> AnonymousPageRequest * { return &m_anonymous; }
      private:
        AnonymousPageRequest m_anonymous;
    };

    class PageSource: public ustl::RefCounter<PageSource> {
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_PAGE_REQUEST_HPP