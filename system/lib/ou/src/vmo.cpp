/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ou/vmo.hpp>

#include <ours/vdso.hpp>

namespace ou {
auto vmo_create(u64 const size, ours::VmoOptions const options, ours::HandleValue *const out)
    -> ours::Status {
    return ours_vmo_create(size, options, out);
}

auto vmo_read(ours::HandleValue const vmo, void *const buffer, u64 const offset, usize const length)
    -> ours::Status {
    return ours_vmo_read(vmo, buffer, offset, length);
}

auto vmo_write(ours::HandleValue const vmo, void const *const buffer, u64 const offset,
               usize const length) -> ours::Status {
    return ours_vmo_write(vmo, buffer, offset, length);
}
} // namespace ou
