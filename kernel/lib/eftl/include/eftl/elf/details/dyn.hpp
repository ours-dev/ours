/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_DETAILS_DYN_HPP
#define EFTL_ELF_DETAILS_DYN_HPP 1

#include <eftl/elf/details/types.hpp>
#include <eftl/elf/observable.hpp>

namespace eftl::elf {
// ELF ABI field names intentionally match the specification.
// NOLINTBEGIN(readability-identifier-naming)
enum class DynTag : u64 {
    Unknown = u64(-1),
    DtNull = 0,          /**< Marks end of dynamic array. */
    Needed = 1,          /**< String table offset of needed library. */
    PltRelSz = 2,        /**< Size of relocation entries in PLT. */
    PltGot = 3,          /**< Address associated with linkage table. */
    Hash = 4,            /**< Address of symbolic hash table. */
    StrTab = 5,          /**< Address of dynamic string table. */
    SymTab = 6,          /**< Address of dynamic symbol table. */
    Rela = 7,            /**< Address of relocation table (Rela entries). */
    RelaSz = 8,          /**< Size of Rela relocation table. */
    RelaEnt = 9,         /**< Size of a Rela relocation entry. */
    StrSz = 10,          /**< Total size of the string table. */
    Syment = 11,         /**< Size of a symbol table entry. */
    Init = 12,           /**< Address of initialization function. */
    Fini = 13,           /**< Address of termination function. */
    SoName = 14,         /**< String table offset of a shared objects name. */
    Rpath = 15,          /**< String table offset of library search path. */
    Symbolic = 16,       /**< Changes symbol resolution algorithm. */
    Rel = 17,            /**< Address of relocation table (Rel entries). */
    RelSz = 18,          /**< Size of Rel relocation table. */
    RelEnt = 19,         /**< Size of a Rel relocation entry. */
    PltRel = 20,         /**< Type of relocation entry used for linking. */
    DebugTag = 21,       /**< Reserved for debugger. */
    TextRel = 22,        /**< Relocations exist for non-writable segments. */
    JmpRel = 23,         /**< Address of relocations associated with PLT. */
    BindNow = 24,        /**< Process all relocations before execution. */
    InitArray = 25,      /**< Pointer to array of initialization functions. */
    FiniArray = 26,      /**< Pointer to array of termination functions. */
    InitArraySz = 27,    /**< Size of DT_INIT_ARRAY. */
    FiniArraySz = 28,    /**< Size of DT_FINI_ARRAY. */
    RunPath = 29,        /**< String table offset of lib search path. */
    Flags = 30,          /**< Flags. */
    PreinitArray = 32,   /**< Pointer to array of preinit functions. */
    PreinitArraySz = 33, /**< Size of the DT_PREINIT_ARRAY array. */
    SymtabShndx = 34,    /**< Address of SYMTAB_SHNDX section */
    RelrSz = 35,         /**< Total size of RELR relative relocations */
    Relr = 36,           /**< Address of RELR relative relocations */
    RelrEnt = 37,        /**< Size of one RELR relative relocaction */

    // GNU Extensions
    GnuHash = 0x6FFFFEF5,    /**< Reference to the GNU hash table. */
    RelaCount = 0x6FFFFFF9,  /**< ELF32_Rela count. */
    RelCount = 0x6FFFFFFA,   /**< ELF32_Rel count. */
    Flags1 = 0x6FFFFFFB,     /**< Flags_1. */
    VerSym = 0x6FFFFFF0,     /**< The address of .gnu.version section. */
    VerDef = 0x6FFFFFFC,     /**< The address of the version definition table. */
    VerDefNum = 0x6FFFFFFD,  /**< The number of entries in DT_VERDEF. */
    VerNeed = 0x6FFFFFFE,    /**< The address of the version Dependency table. */
    VerNeedNum = 0x6FFFFFFF, /**< The number of entries in DT_VERNEED. */
};

template <typename Traits, typename Derived>
struct DynBase: public Observable<Traits, Derived> {
    FORCE_INLINE [[nodiscard]]
    constexpr auto tag() const noexcept -> DynTag {
        return DynTag(static_cast<Derived const *>(this)->d_tag);
    };

    FORCE_INLINE [[nodiscard]]
    constexpr auto value() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->d_val);
    };
};

/// Dynamic table entry for ELF32.
struct Elf32Dyn: public DynBase<Traits32, Elf32Dyn> {
    Elf32Sword d_tag; //< Type of dynamic table entry.
    union {
        Elf32Word d_val; //< Integer value of entry.
        Elf32Addr d_ptr; //< Pointer value of entry.
    };
};

/// Dynamic table entry for ELF64.
struct Elf64Dyn: public DynBase<Traits64, Elf64Dyn> {
    Elf64Sxword d_tag; //< Type of dynamic table entry.
    union {
        Elf64Xword d_val; //< Integer value of entry.
        Elf64Addr d_ptr;  //< Pointer value of entry.
    };
};

static_assert(sizeof(Elf32Dyn) == 8);
static_assert(sizeof(Elf64Dyn) == 16);
// NOLINTEND(readability-identifier-naming)
} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_DETAILS_DYN_HPP
