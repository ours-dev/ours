/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_BUFFER_HPP
#define PROF_BUFFER_HPP 1

#include <prof/format.hpp>
#include <prof/record_buffer.hpp>

namespace prof {

/// The sampler's per-CPU arena: 128 records deep, which at the default
/// periods is a few milliseconds of samples per CPU between drains.
using SampleBuffer = RecordBuffer<SampleRecord, 128>;

} // namespace prof

#endif // #ifndef PROF_BUFFER_HPP
