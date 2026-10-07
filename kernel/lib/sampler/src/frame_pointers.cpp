/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <sampler/frame_pointers.hpp>

namespace sampler {
auto walk_frame_pointers(u64 const pc, u64 fp, unwind::MemoryReader const &reader, u64 *const ips,
                         usize const max) -> usize {
    if (max == 0) {
        return 0;
    }
    ips[0] = pc;
    usize depth = 1;
    while (depth < max) {
        if (fp == 0 || (fp % alignof(u64)) != 0 || fp > ~u64{0} - 2 * sizeof(u64)) {
            break;
        }
        u64 next = 0;
        u64 ret = 0;
        if (!reader.read(reader.context, fp, &next) ||
            !reader.read(reader.context, fp + sizeof(u64), &ret) || ret == 0) {
            break;
        }
        ips[depth] = ret;
        ++depth;
        // Frames sit at higher addresses the further out they are; one that
        // does not is a corrupt chain, or a loop.
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    return depth;
}
} // namespace sampler
