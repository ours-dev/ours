/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_DETAILS_SEGMENT_HPP
#define EFTL_ELF_DETAILS_SEGMENT_HPP 1

#include <eftl/elf/details/types.hpp>
#include <eftl/elf/observable.hpp>

#include <cstddef>

namespace eftl::elf {
// ELF ABI field names intentionally match the specification.
// NOLINTBEGIN(readability-identifier-naming)
enum class SegPerms : unsigned {
    None = 0,
    Executable = 1u << 0, // X
    Writable = 1u << 1,   // W
    Readable = 1u << 2,   // R
};

[[nodiscard]]
constexpr auto operator|(SegPerms lhs, SegPerms rhs) noexcept -> SegPerms {
    return static_cast<SegPerms>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

[[nodiscard]]
constexpr auto operator&(SegPerms lhs, SegPerms rhs) noexcept -> SegPerms {
    return static_cast<SegPerms>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

enum class SegType : u64 {
    Unknown = u64(-1),
    Null = 0,    //< Unused segment.
    Load = 1,    //< Loadable segment.
    Dynamic = 2, //< Dynamic linking information.
    Interp = 3,  //< Interpreter pathname.
    Note = 4,    //< Auxiliary information.
    Shlib = 5,   //< Reserved.
    Phdr = 6,    //< The program header table itself.
    Tls = 7,     //< The thread-local storage template.

    GnuEhFrame = 0x6474e550,
    GnuStack = 0x6474e551,    //< Indicates stack executability.
    GnuProperty = 0x6474e553, //< GNU property.
    GnuRelro = 0x6474e552,    //< Read-only after relocation.
    PaxFlags = 0x65041580,
};

/// CRTP class help us define a group of consistent interface.
template <typename Traits, typename Derived>
struct SegmentBase: public Observable<Traits, Derived> {
    [[nodiscard]]
    FORCE_INLINE constexpr auto va_begin() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_vaddr);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto va_limit() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_vaddr) +
               static_cast<usize>(static_cast<Derived const *>(this)->p_memsz);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto va_size() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_memsz);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto file_offset() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_offset);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto file_size() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_filesz);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto alignment() const noexcept -> usize {
        return static_cast<usize>(static_cast<Derived const *>(this)->p_align);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto permissions() const noexcept -> SegPerms {
        return static_cast<SegPerms>(static_cast<Derived const *>(this)->p_flags);
    }

    [[nodiscard]]
    FORCE_INLINE constexpr auto type() const noexcept -> SegType {
        return static_cast<SegType>(static_cast<Derived const *>(this)->p_type);
    }
};

/** Program header for ELF32. */
struct Elf32Phdr: public SegmentBase<Traits32, Elf32Phdr> {
    Elf32Word p_type;   /**< Type of segment */
    Elf32Off p_offset;  /**< File offset where segment is located, in bytes */
    Elf32Addr p_vaddr;  /**< Virtual address of beginning of segment */
    Elf32Addr p_paddr;  /**< Physical address of beginning of segment (OS-specific) */
    Elf32Word p_filesz; /**< Num. of bytes in file image of segment (may be zero) */
    Elf32Word p_memsz;  /**< Num. of bytes in mem image of segment (may be zero) */
    Elf32Word p_flags;  /**< Segment flags */
    Elf32Word p_align;  /**< Segment alignment constraint */
};

/** Program header for ELF64. */
struct Elf64Phdr: public SegmentBase<Traits64, Elf64Phdr> {
    Elf64Word p_type;    /**< Type of segment */
    Elf64Word p_flags;   /**< Segment flags */
    Elf64Off p_offset;   /**< File offset where segment is located, in bytes */
    Elf64Addr p_vaddr;   /**< Virtual address of beginning of segment */
    Elf64Addr p_paddr;   /**< Physical addr of beginning of segment (OS-specific) */
    Elf64Xword p_filesz; /**< Num. of bytes in file image of segment (may be zero) */
    Elf64Xword p_memsz;  /**< Num. of bytes in mem image of segment (may be zero) */
    Elf64Xword p_align;  /**< Segment alignment constraint */
};

static_assert(sizeof(Elf32Phdr) == 32);
static_assert(sizeof(Elf64Phdr) == 56);
static_assert(offsetof(Elf32Phdr, p_offset) == 4);
static_assert(offsetof(Elf64Phdr, p_offset) == 8);

// NOLINTEND(readability-identifier-naming)

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_DETAILS_SEGMENT_HPP
