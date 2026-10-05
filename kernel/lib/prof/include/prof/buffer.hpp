/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_BUFFER_HPP
#define PROF_BUFFER_HPP 1

#include <prof/format.hpp>
#include <prof/record_buffer.hpp>

namespace prof {

/// The sampler's per-CPU arena. Its depth is whatever slots the session
/// attaches, sized from the buffer size the session was created with.
using SampleBuffer = RecordBuffer<SampleRecord>;

} // namespace prof

#endif // #ifndef PROF_BUFFER_HPP
