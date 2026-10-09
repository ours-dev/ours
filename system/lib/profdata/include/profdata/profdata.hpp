/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// PROVENANCE: adapted from Fuchsia's src/lib/llvm-profdata (Copyright 2021 The
/// Fuchsia Authors, BSD-style license).
#ifndef PROFDATA_PROFDATA_HPP
#define PROFDATA_PROFDATA_HPP 1

#include <span>

#include <cstddef>

namespace profdata {

/// The raw profile of a program's own source-based coverage counters, in the
/// format `llvm-profdata` reads.
///
/// A program built with `-fprofile-instr-generate -fcoverage-mapping` counts
/// in memory the compiler laid out, and the compiler's own runtime writes that
/// memory to a file when the program exits. A kernel has no such runtime and no
/// exit, so this class does the part that is not I/O: it knows how big the
/// profile is, writes everything in it that does not change while the program
/// runs, and copies the counters in when asked. Where the bytes go afterwards
/// is the caller's business.
///
/// With `-mllvm -profile-correlate=binary` the compiler leaves its function
/// records and names in the binary instead of the counters' memory, so the
/// profile holds little more than a header, a build ID and the counters, and
/// `llvm-profdata merge --binary-file=<the binary>` finds the rest by the build
/// ID. Without it they are part of the profile. Either way the layout is
/// whatever the compiler's own `InstrProfData.inc` says, which is where this
/// file takes it from.
///
/// The object is a snapshot of sizes only. It reads no counter at construction,
/// so it can be made once and used for as many dumps as wanted.
class RawProfile {
  public:
    /// The profile's alignment: the buffer given to write_fixed() must start on
    /// a multiple of this, because the counters in it are 64-bit.
    static constexpr usize ALIGNMENT = 8;

    /// The parts of a dump that hold counter values rather than fixed text,
    /// as spans into the dump buffer. Each is empty when the program has none
    /// of that kind.
    struct LiveRegions {
        std::span<std::byte> counters;
        std::span<std::byte> bitmap;
        std::span<std::byte> uniform_counters;
    };

    /// \param build_id the identity of the binary being profiled, as the linker
    ///        recorded it. It is written into the profile and is how
    ///        llvm-profdata pairs a profile with a binary. It is borrowed, not
    ///        copied, and must outlive this object.
    explicit RawProfile(std::span<std::byte const> build_id);

    /// The size in bytes of a dump, or zero when nothing in the program was
    /// instrumented (and so there is nothing to write).
    [[nodiscard]]
    auto size_bytes() const -> usize;

    /// Writes the header, the build ID and whatever else is fixed for the life
    /// of the program into `dump`, which must be size_bytes() long and aligned
    /// to ALIGNMENT, and returns where in it the counters belong. The counters
    /// themselves are left untouched: they change under the program's feet, so
    /// copy_live() takes them at the moment the caller picks.
    [[nodiscard]]
    auto write_fixed(std::span<std::byte> dump) const -> LiveRegions;

    /// Copies the program's counters into the regions write_fixed() returned.
    ///
    /// Other CPUs may be running instrumented code while this runs, and they
    /// increment the counters without synchronisation, so what is copied is
    /// each counter as it was at some moment during the call, not all of them
    /// at one moment. That is enough for what a counter is used for here: it
    /// only grows, so a copy never shows a count that was not reached, and a
    /// count that was missed shows up in the next copy. Each counter is read
    /// with a relaxed atomic load, so the read cannot tear or be reordered away
    /// by the compiler, which a plain copy would allow.
    auto copy_live(LiveRegions const &regions) const -> void;

  private:
    std::span<std::byte const> m_build_id;
    usize m_size_bytes{0};
    usize m_counters_offset{0};
    usize m_counters_size_bytes{0};
    usize m_bitmap_offset{0};
    usize m_bitmap_size_bytes{0};
    usize m_uniform_counters_offset{0};
    usize m_uniform_counters_size_bytes{0};
};

} // namespace profdata

#endif // #ifndef PROFDATA_PROFDATA_HPP
