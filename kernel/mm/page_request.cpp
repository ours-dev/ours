/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/page_request.hpp>

namespace ours::mm {
/// Blocks until the pages this request asked for have been supplied.
///
/// TODO(SmallHuaZi): nothing can raise an asynchronous request yet -- no
/// PageSource is implemented, and VmCowPages::Cursor::create_read_request()
/// returns Unimplemented -- so there is never anything to wait for. The
/// definition exists because VmAspace::fault() already calls it on the
/// Status::ShouldWait path, which is unreachable for the same reason.
auto PageRequest::wait(bool suspendable) -> Status {
    return Status::Unimplemented;
}

auto MultiPageRequest::wait(bool suspendable) -> Status {
    return m_read.wait(suspendable);
}
} // namespace ours::mm
