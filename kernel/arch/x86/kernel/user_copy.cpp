/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/arch/user_copy.hpp>
#include <ours/arch/x86/feature.hpp>

// The copy itself, in user_copy.S: one routine per SMAP setting, so that the
// stac/clac window is written out and holds the copy and nothing else. See the
// file's header for why it is not memcpy.
extern "C" auto x86_user_copy(void *dst, void const *src, usize len) -> void;
extern "C" auto x86_user_copy_smap(void *dst, void const *src, usize len) -> void;

namespace ours {
namespace {
    /// The variant this CPU can run: the plain one where stac/clac would be
    /// #UD, the bracketed one where SMAP would otherwise refuse the access.
    auto copy(void *const dst, void const *const src, usize const size) -> void {
        if (feature_has_smap) {
            x86_user_copy_smap(dst, src, size);
        } else {
            x86_user_copy(dst, src, size);
        }
    }
} // namespace

auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> void {
    copy(dst, reinterpret_cast<void const *>(src), size);
}

auto arch_copy_to_user(VirtAddr const dst, void const *const src, usize const size) -> void {
    copy(reinterpret_cast<void *>(dst), src, size);
}

} // namespace ours
