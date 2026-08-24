#ifndef EFTL_ELF_TRAITS_HPP
#define EFTL_ELF_TRAITS_HPP

#include <eftl/elf/details/structures.hpp>
#include <eftl/elf/details/types.hpp>

namespace eftl::elf {
enum class Class {
    None,
    Elf32,
    Elf64,
};

[[nodiscard]]
constexpr auto native_elf_class() noexcept -> Class {
    if (sizeof(void *) == 8) {
        return Class::Elf64;
    }
    return Class::Elf32;
}

template <Class K>
struct TraitsDispatcher;

struct ElfSiginfo;
struct ClassTraitsBase {
    static constexpr char K_ELF_MAGIC[] = {'\x7f', 'E', 'L', 'F'};

    using Siginfo = ElfSiginfo; // Signal information
};

struct Elf32Ehdr;
struct Elf32Phdr;
struct Elf32Shdr;
struct Elf32Chdr;
struct Elf32Sym;
struct Elf32Auxv;
struct Elf32Dyn;
struct Elf32Rel;
struct Elf32Rela;
struct Elf32Verneed;
struct Elf32Verdef;
struct Elf32Verdaux;
struct Elf32Vernaux;
struct Elf32Prstatus;
struct Elf32FileEntry;
struct Elf32Timeval;

struct Traits32: public ClassTraitsBase {
    static constexpr u8 K_ELF_CLASS = 1;
    using Addr = Elf32Addr;
    using Off = Elf32Off;
    using Half = Elf32Half;
    using Word = Elf32Word;
    using Sword = Elf32Sword;

    using Header = Elf32Ehdr;         // Elf header
    using Segment = Elf32Phdr;        // Program header
    using Section = Elf32Shdr;        // Section header
    using Compression = Elf32Chdr;    // Compressed section header
    using Symbol = Elf32Sym;          // Symbol table entry
    using Auxv = Elf32Auxv;           // Auxiliary vector
    using Dyn = Elf32Dyn;             // Dynamic table entry
    using Rel = Elf32Rel;             // Relocation entry
    using Rela = Elf32Rela;           // Relocation entry with explicit addend
    using Verneed = Elf32Verneed;     // Version needed
    using Verdef = Elf32Verdef;       // Version definition
    using Verdaux = Elf32Verdaux;     // Version definition auxiliary
    using Vernaux = Elf32Vernaux;     // Version auxiliary
    using Prstatus = Elf32Prstatus;   // Prstatus structure
    using FileEntry = Elf32FileEntry; // File entry
    using Timeval = Elf32Timeval;     // Time value

    [[nodiscard]]
    static constexpr auto relocation_type(Word info) noexcept -> u32 {
        return info & 0xffU;
    }

    [[nodiscard]]
    static constexpr auto relocation_symbol(Word info) noexcept -> u32 {
        return info >> 8;
    }
};

template <>
struct TraitsDispatcher<Class::Elf32> {
    using Type = Traits32;
};

struct Elf64Ehdr;
struct Elf64Phdr;
struct Elf64Shdr;
struct Elf64Chdr;
struct Elf64Sym;
struct Elf64Auxv;
struct Elf64Dyn;
struct Elf64Rel;
struct Elf64Rela;
struct Elf64Verneed;
struct Elf64Verdef;
struct Elf64Verdaux;
struct Elf64Vernaux;
struct Elf64Prstatus;
struct Elf64FileEntry;
struct Elf64Timeval;

struct Traits64: public ClassTraitsBase {
    static constexpr u8 K_ELF_CLASS = 2;
    using Addr = Elf64Addr;
    using Off = Elf64Off;
    using Half = Elf64Half;
    using Word = Elf64Word;
    using Sword = Elf64Sword;
    using Xword = Elf64Xword;

    using Header = Elf64Ehdr;         // Elf header
    using Segment = Elf64Phdr;        // Program header
    using Section = Elf64Shdr;        // Section header
    using Compression = Elf64Chdr;    // Compressed section header
    using Symbol = Elf64Sym;          // Symbol table entry
    using Auxv = Elf64Auxv;           // Auxiliary vector
    using Dyn = Elf64Dyn;             // Dynamic table entry
    using Rel = Elf64Rel;             // Relocation entry
    using Rela = Elf64Rela;           // Relocation entry with explicit addend
    using Verneed = Elf64Verneed;     // Version needed
    using Verdef = Elf64Verdef;       // Version definition
    using Verdaux = Elf64Verdaux;     // Version definition auxiliary
    using Vernaux = Elf64Vernaux;     // Version auxiliary
    using Prstatus = Elf64Prstatus;   // Prstatus structure
    using FileEntry = Elf64FileEntry; // File entry
    using Timeval = Elf64Timeval;     // Time value

    [[nodiscard]]
    static constexpr auto relocation_type(Xword info) noexcept -> u32 {
        return static_cast<u32>(info & 0xffffffffU);
    }

    [[nodiscard]]
    static constexpr auto relocation_symbol(Xword info) noexcept -> u32 {
        return static_cast<u32>(info >> 32);
    }
};

template <>
struct TraitsDispatcher<Class::Elf64> {
    using Type = Traits64;
};

using TraitsNative = TraitsDispatcher<native_elf_class()>::Type;

} // namespace eftl::elf

#endif // EFTL_ELF_TRAITS_HPP
