/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/log.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

#include <limits>

#include <syscall/handlers.hpp>

namespace ours {
namespace {
    /// A ceiling on one call, so that a user program cannot hold a CPU inside
    /// the kernel for as long as it likes by asking for a gigabyte of output.
    /// The number is arbitrary; what is not arbitrary is that there is one.
    constexpr auto MAX_DEBUG_WRITE = usize{256};
} // namespace

auto sys_debug_write(char const *const buffer, usize const length) -> Status {
    if (length == 0) {
        return Status::Ok;
    }
    if (length > MAX_DEBUG_WRITE) {
        return Status::OutOfRange;
    }

    // The whole range, not just its first byte. A buffer that starts in user
    // space and runs past the split would otherwise let a user program have
    // the kernel read its own memory out to the console one byte at a time.
    // The end is computed as a length rather than as `start + length` first,
    // because that sum can wrap -- and a wrapped pointer would pass a naive
    // range check by landing back below the split.
    auto const start = reinterpret_cast<VirtAddr>(buffer);
    if (mm::is_kernel_address(start) || length > std::numeric_limits<VirtAddr>::max() - start ||
        mm::is_kernel_address(start + length - 1)) {
        return Status::InvArgs;
    }

    // Read straight through the user pointer, which works because this runs on
    // the calling thread with its own address space still installed, and
    // because CR4.SMAP is not enabled (x86_init_percpu sets only FsGsBase and
    // Umip). Neither is a good long-term answer: a proper copy_from_user has
    // to survive the page under the pointer being unmapped by another thread
    // between the check and the read, and that needs a fault handler this
    // kernel does not have yet. It is written up in the change record rather
    // than pretended away.
    //
    // The user's bytes are an argument, never the format string: passing them
    // as a format would let a user program direct the formatter with its own
    // data.
    log::info("{}", ustl::views::StringView{buffer, length});
    return Status::Ok;
}
} // namespace ours
