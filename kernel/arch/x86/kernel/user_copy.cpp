/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/user_copy.hpp>
#include <ours/arch/x86/feature.hpp>

#include <cstring>

namespace ours {
namespace {
    /// The span of code in which ring 0 may touch user pages.
    ///
    /// On a CPU with SMAP that is exactly the span where RFLAGS.AC is set, and
    /// this is the only place in the kernel that sets it. An object rather than
    /// a stac/clac pair written out at each site, so that the clac cannot be
    /// forgotten on an early return.
    ///
    /// Both instructions carry a "memory" clobber: without it the compiler is
    /// free to move the copy's loads and stores across them, and a store the
    /// optimiser sinks past the clac is a store SMAP refuses.
    ///
    /// An interrupt taken inside the window does not inherit it: every entry
    /// stub begins with clac (idt.S), and iretq puts AC back from the frame on
    /// the way out. A page fault the copy itself raises is resolved the same
    /// way, with AC clear, and the copy resumes with it set.
    ///
    /// On a CPU without SMAP both instructions are #UD, and there is nothing
    /// to open: the window is empty.
    ///
    /// Linux counterpart: __uaccess_begin()/__uaccess_end() (stac()/clac()).
    class UserAccessWindow final {
      public:
        UserAccessWindow() {
            if (feature_has_smap) {
                asm volatile("stac" ::: "memory");
            }
        }

        ~UserAccessWindow() {
            if (feature_has_smap) {
                asm volatile("clac" ::: "memory");
            }
        }

        UserAccessWindow(UserAccessWindow const &) = delete;
        auto operator=(UserAccessWindow const &) -> UserAccessWindow & = delete;
    };
} // namespace

auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> void {
    UserAccessWindow const window;
    std::memcpy(dst, reinterpret_cast<void const *>(src), size);
}

auto arch_copy_to_user(VirtAddr const dst, void const *const src, usize const size) -> void {
    UserAccessWindow const window;
    std::memcpy(reinterpret_cast<void *>(dst), src, size);
}

} // namespace ours
