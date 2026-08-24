/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <lib/code-patching/code-patching.hpp>

#include <cstdint>
#include <cstring>

namespace code_patching {
namespace {

/// Architecture-appropriate NOP byte.  0x90 is the single-byte NOP on
/// x86 and a harmless undefined instruction on AArch64 — execution
/// never reaches it after a successful patch.
constexpr uint8_t NOP_BYTE = 0x90;

/// Find the alternative blob for `case_id`, or nullptr if none exists.
auto find_blob(CaseId case_id,
               ustl::views::Span<const CodePatchBlob> blobs)
    -> const CodePatchBlob * {
    for (const auto &blob : blobs) {
        if (blob.case_id == case_id) {
            return &blob;
        }
    }
    return nullptr;
}

/// Replace the code at `entry.patch_site` with `blob->code`, then
/// NOP-fill the remaining bytes up to `entry.max_size`.
auto apply_one(const CodePatchEntry &entry,
               const CodePatchBlob &blob,
               const ArchCodePatchHooks &hooks) -> void {
    auto *const site = reinterpret_cast<uint8_t *>(entry.patch_site);

    hooks.make_writable(entry.patch_site, entry.max_size);

    std::memcpy(site, blob.code, blob.size);

    if (blob.size < entry.max_size) {
        std::memset(site + blob.size, NOP_BYTE, entry.max_size - blob.size);
    }

    hooks.make_executable(entry.patch_site, entry.max_size);
    hooks.sync_icache(entry.patch_site, entry.max_size);
}

}  // namespace

auto apply_code_patches(ustl::views::Span<const CodePatchEntry> entries,
                        ustl::views::Span<const CodePatchBlob> blobs,
                        const ArchCodePatchHooks &hooks) -> size_t {
    size_t applied = 0;

    for (const auto &entry : entries) {
        // case_id == 0 means "no alternative defined for this site".
        if (entry.case_id == 0) {
            continue;
        }

        // Let the architecture decide whether this alternative applies
        // on the current machine.  Fall back to a simple linear search
        // when no decision hook is provided.
        const CodePatchBlob *blob = nullptr;
        if (hooks.select_alternative) {
            blob = hooks.select_alternative(entry.case_id, blobs);
        } else {
            blob = find_blob(entry.case_id, blobs);
        }

        if (!blob || blob->size == 0) {
            continue;
        }

        // The build system guarantees blob->size <= entry.max_size.
        // If this fails something went wrong during packaging — skip
        // the entry rather than corrupting memory.
        if (blob->size > entry.max_size) {
            continue;
        }

        apply_one(entry, *blob, hooks);
        ++applied;
    }

    return applied;
}

}  // namespace code_patching
