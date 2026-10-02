/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// Program::read(): the PT_LOAD segments, the PT_GNU_RELRO, the PT_GNU_STACK
/// size and the entry point, read out of ELF images built here byte by byte.
#include <launch/plan.hpp>
#include <launch/program.hpp>

#include <vector>

#include <cstring>

#include <eftl/elf/binary.hpp>
#include <eftl/elf/enums.hpp>
#include <eftl/elf/traits.hpp>
#include <gtest/gtest.h>

namespace {
using launch::PF_R;
using launch::PF_W;
using launch::PF_X;
using launch::Plan;
using launch::Program;
using ours::Status;

using Traits = eftl::elf::Traits64;
using Header = Traits::Header;
using Phdr = Traits::Segment;

constexpr u16 ET_DYN = 3;

auto header_of(u32 const type, u32 const flags, u64 const offset, u64 const vaddr,
               u64 const file_size, u64 const mem_size) -> Phdr {
    Phdr header{};
    header.p_type = type;
    header.p_flags = flags;
    header.p_offset = offset;
    header.p_vaddr = vaddr;
    header.p_paddr = vaddr;
    header.p_filesz = file_size;
    header.p_memsz = mem_size;
    header.p_align = 0x1000;
    return header;
}

auto load(u32 const flags, u64 const offset, u64 const vaddr, u64 const file_size,
          u64 const mem_size) -> Phdr {
    return header_of(static_cast<u32>(eftl::elf::SegType::Load), flags, offset, vaddr, file_size,
                     mem_size);
}

auto relro(u64 const offset, u64 const vaddr, u64 const file_size, u64 const mem_size) -> Phdr {
    return header_of(static_cast<u32>(eftl::elf::SegType::GnuRelro), PF_R, offset, vaddr, file_size,
                     mem_size);
}

/// A PT_GNU_STACK asking for |size| bytes of stack; 0, what a linker writes
/// unless told -z stack-size, leaves the size to whoever starts the program.
auto gnu_stack(u64 const size = 0) -> Phdr {
    return header_of(static_cast<u32>(eftl::elf::SegType::GnuStack), PF_R | PF_W, 0, 0, 0, size);
}

/// An x86-64 ET_DYN of |file_size| bytes: the ELF header, then |headers| as
/// its program headers, then zeros.
auto elf_of(std::vector<Phdr> const &headers, u64 const entry, usize const file_size)
    -> std::vector<u8> {
    std::vector<u8> file(file_size);
    Header elf{};
    std::memcpy(elf.e_ident, eftl::elf::ELF_MAGIC, sizeof(eftl::elf::ELF_MAGIC));
    elf.e_ident[4] = Traits::ELF_CLASS;
    elf.e_ident[5] = 1; // little-endian
    elf.e_ident[6] = 1; // EV_CURRENT
    elf.e_type = ET_DYN;
    elf.e_machine = static_cast<u16>(eftl::elf::Arch::X86_64);
    elf.e_version = 1;
    elf.e_entry = entry;
    elf.e_phoff = sizeof(Header);
    elf.e_ehsize = sizeof(Header);
    elf.e_phentsize = sizeof(Phdr);
    elf.e_phnum = static_cast<u16>(headers.size());
    std::memcpy(file.data(), &elf, sizeof(elf));
    for (usize i = 0; i < headers.size(); ++i) {
        std::memcpy(file.data() + sizeof(Header) + i * sizeof(Phdr), &headers[i], sizeof(Phdr));
    }
    return file;
}

/// What LLD gives a small PIE: headers and read-only data, code, and a writable
/// segment that starts with RELRO and ends in .bss. The RELRO's p_filesz is
/// shorter than its p_memsz, as .relro_padding makes it.
auto typical() -> std::vector<u8> {
    return elf_of(
        {
            load(PF_R, 0x0000, 0x0000, 0x600, 0x600),
            load(PF_R | PF_X, 0x1000, 0x1000, 0x340, 0x340),
            load(PF_R | PF_W, 0x2f00, 0x3f00, 0x180, 0x2180),
            relro(0x2f00, 0x3f00, 0x80, 0x100),
            gnu_stack(),
        },
        0x1000, 0x3080);
}

#define PROGRAM_OF(name, file)                                                                     \
    auto const name##_bytes = (file);                                                              \
    auto const name##_result = Program::read(name##_bytes);                                        \
    ASSERT_TRUE(name##_result.is_ok())                                                             \
        << "read() answered " << static_cast<int>(name##_result.unwrap_err());                     \
    auto const &name = name##_result.unwrap()

void expect_refused(std::vector<u8> const &file, Status const status) {
    auto const result = Program::read(file);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), status);
}

TEST(ProgramRead, LoadSegmentsInFileOrder) {
    PROGRAM_OF(program, typical());
    ASSERT_EQ(program.segments().size(), 3U);
    auto const expect = [&](usize const i, u64 const vaddr, u64 const offset, u64 const file_size,
                            u64 const mem_size, u32 const flags) {
        auto const &segment = program.segments()[i];
        EXPECT_EQ(segment.vaddr, vaddr) << "segment " << i;
        EXPECT_EQ(segment.file_offset, offset) << "segment " << i;
        EXPECT_EQ(segment.file_size, file_size) << "segment " << i;
        EXPECT_EQ(segment.mem_size, mem_size) << "segment " << i;
        EXPECT_EQ(segment.flags, flags) << "segment " << i;
    };
    expect(0, 0x0000, 0x0000, 0x600, 0x600, PF_R);
    expect(1, 0x1000, 0x1000, 0x340, 0x340, PF_R | PF_X);
    expect(2, 0x3f00, 0x2f00, 0x180, 0x2180, PF_R | PF_W);
    EXPECT_EQ(program.entry(), 0x1000U);
}

TEST(ProgramRead, RelroSizeIsItsMemsz) {
    // p_filesz is 0x80 and p_memsz 0x100: the size is the one that reaches the
    // end of the last RELRO page.
    PROGRAM_OF(program, typical());
    EXPECT_EQ(program.relro().vaddr, 0x3f00U);
    EXPECT_EQ(program.relro().size, 0x100U);
}

TEST(ProgramRead, NoRelroIsSizeZero) {
    PROGRAM_OF(program, elf_of({load(PF_R | PF_X, 0x0, 0x0, 0x100, 0x100)}, 0x10, 0x100));
    EXPECT_EQ(program.relro().size, 0U);
    EXPECT_EQ(program.segments().size(), 1U);
}

TEST(ProgramRead, StackSizeIsGnuStacksMemsz) {
    PROGRAM_OF(
        asks, elf_of({load(PF_R | PF_X, 0x0, 0x0, 0x100, 0x100), gnu_stack(0x40000)}, 0x10, 0x100));
    EXPECT_EQ(asks.stack_size(), 0x40000U);
    // typical() has a PT_GNU_STACK that names no size, and this one has none.
    PROGRAM_OF(leaves, typical());
    EXPECT_EQ(leaves.stack_size(), 0U);
    PROGRAM_OF(none, elf_of({load(PF_R | PF_X, 0x0, 0x0, 0x100, 0x100)}, 0x10, 0x100));
    EXPECT_EQ(none.stack_size(), 0U);
}

TEST(ProgramRead, ItsSegmentsMakeAPlan) {
    // What the launcher will do with it: the numbers go to Plan::build as they
    // are, with the file's size.
    // Two mappings from the file (R, RX), and the writable segment's two: its
    // page of file bytes from the snapshot, read-only since it is wholly RELRO
    // by p_memsz, and the page its data ends in with the .bss after it.
    PROGRAM_OF(program, typical());
    auto const plan = Plan::build(program.segments(), program.relro(), typical().size());
    ASSERT_TRUE(plan.is_ok());
    EXPECT_EQ(plan.unwrap().mappings().size(), 4U);
}

TEST(ProgramRead, RefusesTwoRelros) {
    expect_refused(elf_of(
                       {
                           load(PF_R | PF_X, 0x0, 0x0, 0x100, 0x100),
                           load(PF_R | PF_W, 0x1000, 0x1000, 0x2000, 0x2000),
                           relro(0x1000, 0x1000, 0x800, 0x800),
                           relro(0x2000, 0x2000, 0x800, 0x800),
                       },
                       0x10, 0x3000),
                   Status::NotSupported);
}

TEST(ProgramRead, RefusesMoreSegmentsThanAPlanTakes) {
    // Nine non-overlapping segments, which the program check accepts (it takes
    // up to twelve) and a Plan does not.
    std::vector<Phdr> headers;
    for (u64 i = 0; i <= Plan::MAX_SEGMENTS; ++i) {
        headers.push_back(load(PF_R | PF_X, i * 0x1000, i * 0x1000, 0x10, 0x10));
    }
    expect_refused(elf_of(headers, 0x0, (Plan::MAX_SEGMENTS + 1) * 0x1000), Status::OutOfRange);
}

TEST(ProgramRead, RefusesWhatIsNotAnElf) {
    expect_refused(std::vector<u8>(0x100, 0xa5), Status::InvArgs);
    // Program headers that run past the end of the file.
    auto truncated = typical();
    truncated.resize(0x80);
    expect_refused(truncated, Status::InvArgs);
}
} // namespace
