/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_PROGRAM_HPP
#define LAUNCH_PROGRAM_HPP 1

#include <launch/plan.hpp>

#include <array>
#include <span>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace launch {
/// What the launcher needs from a program's ELF before it can plan it: the
/// PT_LOAD segments in file order, the PT_GNU_RELRO, and where the program
/// starts. Everything else in the headers -- PT_DYNAMIC for the binder, the
/// section headers, notes -- is read from the file by whoever needs it.
///
/// Read once, by read(), from the whole file: the launcher reads the program
/// out of its VMO into memory before planning it, because binding it against
/// the vDSO reads the file's dynamic tables as well. A Program is not changed
/// after it is read.
///
/// Reading is not judging. Whether the file is a program this system can start
/// -- its machine, its type, an entry point in an executable segment -- is
/// elfload::check_program()'s question, the same one the kernel's loader asks,
/// and the launcher asks it before it reads. read() only takes the numbers out,
/// so it builds and is tested on any host: it reads the ELF as 64-bit and the
/// architecture is not its business.
class Program {
  public:
    /// Reads |file|, the bytes of an ELF program.
    ///
    ///   InvArgs       not a 64-bit ELF whose headers lie inside |file|.
    ///   OutOfRange    more PT_LOAD segments than a Plan takes
    ///                 (Plan::MAX_SEGMENTS).
    ///   NotSupported  more than one PT_GNU_RELRO. A linker gives a program one;
    ///                 more come from linker scripts that scatter the RELRO
    ///                 sections (the kernel's own does), and a Plan takes one.
    [[nodiscard]]
    static auto read(std::span<u8 const> file) -> ustl::Result<Program, ours::Status>;

    /// The PT_LOAD segments, in the order the file has them.
    [[nodiscard]]
    auto segments() const -> std::span<Segment const> {
        return {m_segments.data(), m_segment_count};
    }

    /// The PT_GNU_RELRO, with its size taken from p_memsz (see Relro); size 0
    /// if the program has none.
    [[nodiscard]]
    auto relro() const -> Relro {
        return m_relro;
    }

    /// e_entry: the link-time address the program starts at.
    [[nodiscard]]
    auto entry() const -> u64 {
        return m_entry;
    }

  private:
    Program() = default;

    std::array<Segment, Plan::MAX_SEGMENTS> m_segments = {};
    usize m_segment_count = 0;
    Relro m_relro;
    u64 m_entry = 0;
};
} // namespace launch

#endif // #ifndef LAUNCH_PROGRAM_HPP
