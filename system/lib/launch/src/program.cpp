/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/program.hpp>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/enums.hpp>
#include <eftl/elf/parser.hpp>
#include <eftl/elf/traits.hpp>

namespace launch {
/// Traits64 rather than the build machine's own: the programs are x86-64 ELFs
/// whatever this is compiled for, and the host tests run on an arm64 machine.
auto Program::read(std::span<u8 const> const file) -> ustl::Result<Program, ours::Status> {
    using ours::Status;

    using Traits = eftl::elf::Traits64;
    auto const parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    auto const &elf = parsed.value();

    Program program;
    program.m_entry = elf.elf_header().e_entry;
    auto relro_count = usize{0};
    for (auto const header: elf.segments()) {
        if (header.type() == eftl::elf::SegType::Load) {
            if (program.m_segment_count == Plan::MAX_SEGMENTS) {
                return ustl::err(Status::OutOfRange);
            }
            program.m_segments[program.m_segment_count] = Segment{
                .vaddr = header.p_vaddr,
                .file_offset = header.p_offset,
                .file_size = header.p_filesz,
                .mem_size = header.p_memsz,
                .flags = header.p_flags,
            };
            program.m_segment_count += 1;
            continue;
        }
        if (header.type() == eftl::elf::SegType::GnuRelro) {
            relro_count += 1;
            if (relro_count > 1) {
                return ustl::err(Status::NotSupported);
            }
            // p_memsz, not p_filesz: LLD's .relro_padding, which rounds RELRO up
            // to a page, takes no room in the file (see Relro).
            program.m_relro = Relro{.vaddr = header.p_vaddr, .size = header.p_memsz};
        }
    }
    return ustl::ok(program);
}
} // namespace launch
