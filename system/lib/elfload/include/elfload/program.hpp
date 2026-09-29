/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ELFLOAD_PROGRAM_HPP
#define ELFLOAD_PROGRAM_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace elfload {
/// Whether |file| is a program this kernel can start: an ELF for this
/// machine, of type ET_EXEC or ET_DYN, whose PT_LOAD segments do not
/// overlap, and whose entry point lies inside one of them that is
/// executable.
///
/// Asked before anything is built for the program, which is the whole point
/// of it being separate from Loader::map(): a Loader belongs to an address
/// space, so by the time it can say "this is not a program" a process
/// already exists and has to be torn down again. Linux draws the same line
/// in load_elf_binary(), whose header checks all come before
/// begin_new_exec(), the point after which the old image is gone.
///
/// Not asked of the vDSO, whose e_entry means nothing -- nothing jumps to a
/// vDSO -- and which this would rightly refuse.
[[nodiscard]]
auto check_program(ustl::views::Span<u8 const> file) -> ours::Status;
} // namespace elfload

#endif // #ifndef ELFLOAD_PROGRAM_HPP
