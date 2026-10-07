/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef SAMPLER_BUFFER_HPP
#define SAMPLER_BUFFER_HPP 1

#include <sampler/format.hpp>

#include <prof/record_buffer.hpp>

namespace sampler {

/// The sampler's per-CPU arena. Its depth is whatever slots the session
/// attaches, sized from the buffer size the session was created with.
using SampleBuffer = prof::RecordBuffer<SampleRecord>;

} // namespace sampler

#endif // #ifndef SAMPLER_BUFFER_HPP
