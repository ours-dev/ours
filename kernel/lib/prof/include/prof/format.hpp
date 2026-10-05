/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_FORMAT_HPP
#define PROF_FORMAT_HPP 1

#include <ours/sampler.hpp>

namespace prof {

/// The record and the dump header are what sampler_read hands a program, so
/// they are defined with it, in <ours/sampler.hpp>, and the host prof-fold
/// tool reads the same bytes. These are this library's names for them.
inline constexpr auto SAMPLE_MAGIC = ours::SAMPLER_RECORD_MAGIC;
inline constexpr auto MAX_STACK_DEPTH = ours::SAMPLER_MAX_STACK_DEPTH;
using RecordFlags = ours::SamplerRecordFlags;
using SampleRecord = ours::SamplerRecord;

inline constexpr auto DUMP_MAGIC = ours::SAMPLER_DUMP_MAGIC;
inline constexpr auto DUMP_VERSION = ours::SAMPLER_DUMP_VERSION;
using DumpHeader = ours::SamplerDumpHeader;

/// Serial export framing: the header goes out as one `PROF1 <hex>` line and
/// every record as one `SAMP <hex>` line. Line framing survives the texty
/// serial console (and QEMU `-serial file:`) without binary escapes; the
/// magic doubles as a resync point after line corruption.
inline constexpr char DUMP_HEADER_TAG[6] = "PROF1";
inline constexpr char DUMP_RECORD_TAG[5] = "SAMP";

} // namespace prof

#endif // #ifndef PROF_FORMAT_HPP
