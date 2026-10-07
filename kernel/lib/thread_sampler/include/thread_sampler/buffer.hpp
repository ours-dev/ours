/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef THREAD_SAMPLER_BUFFER_HPP
#define THREAD_SAMPLER_BUFFER_HPP 1

#include <thread_sampler/format.hpp>

#include <prof/record_buffer.hpp>

namespace sampler {

/// The sampler's per-CPU arena. Its depth is whatever slots the session
/// attaches, sized from the buffer size the session was created with.
using SampleBuffer = prof::RecordBuffer<SampleRecord>;

} // namespace sampler

#endif // #ifndef THREAD_SAMPLER_BUFFER_HPP
