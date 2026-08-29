/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_PACKAGE_FORMAT_HPP
#define OBI_PACKAGE_FORMAT_HPP 1

#include <ours/types.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace obi {
    static_assert(std::endian::native == std::endian::little,
                  "OBI wire fields are little-endian");

    enum class ItemType : u32 {
        // A compressed or uncompressed BootFS image carrying the phys boot
        // payload (physboot, kernel, userboot, vDSO, code-patches, etc.).
        StorageKernel = 0x4B535452, // KSTR
        // An uncompressed BootFS image mounted as /boot.
        StorageBootFs = 0x42545346, // BTSF
        Executable = 0x45584543, // EXEC
        Plugin = 0x504C5547, // PLUG
        Vdso = 0x5644534F, // VDSO
        Config = 0x434F4E46, // CONF
        Data = 0x44415441, // DATA
    };

    enum class ItemFormat : u32 {
        BootFs = 0x42545346, // BTSF
        Elf = 0x454C4620, // ELF<space>
        Raw = 0x52415720, // RAW<space>
        Yaml = 0x59414D4C, // YAML
        MemConfig = 0x4D454D52, // MEMR
    };

    enum class Compression : u32 {
        None = 0,
        // Raw LZ4 block (no frame header); memory_size is the decoded size.
        Lz4 = 1,
    };

    enum class ItemFlags : u32 {
        None = 0,
        Required = 1u << 0,
        // Reserved OBI vocabulary.  OBI 1.0 rejects these until a consumer
        // owns their lifecycle or memory-management semantics.
        Autoload = 1u << 1,
        Reclaimable = 1u << 2,
        Readonly = 1u << 3,
        Early = 1u << 4,
    };

    inline constexpr auto kSupportedItemFlags = u32 {
        static_cast<u32>(ItemFlags::Required)
    };

    enum class ContainerFlags : u32 {
        None = 0,
        Crc32 = 1u << 0,
    };

    enum class MemoryRangeType : u32 {
        Ram = 1,
        Peripheral = 2,
        Reserved = 3,
    };

    struct ContainerHeader final {
        static constexpr auto kMagic = u32 {0x424F4F54}; // "BOOT", LE
        static constexpr auto kMajor = u16 {1};
        static constexpr auto kMinor = u16 {0};
        static constexpr auto kNoSetupItem = u32 {0xFFFFFFFF};
        static constexpr auto kAlignment = u64 {8};
        // Container validation is allocation-free and performs bounded work
        // per item, so keep boot-time metadata processing bounded for hostile input.
        static constexpr auto kMaxItems = u32 {4096};

        u32 magic;
        u32 reserved0;
        u16 major;
        u16 minor;
        u32 header_size;
        u64 container_size;
        u64 item_table_offset;
        u32 item_count;
        u32 string_table_offset;
        u32 string_table_size;
        u32 setup_item_index;
        u32 flags;
        u8 container_digest[32];
        u32 reserved;
    };

    struct ItemDescriptor final {
        u32 name_offset;
        u16 name_size;
        u16 reserved_name;
        u32 type;
        u32 flags;
        u32 format;
        u32 compression;
        u32 reserved;
        u64 payload_offset;
        u64 payload_size;
        u64 memory_size;
        u64 alignment;
        u8 digest[32];
    };

    struct MemoryRange final {
        u64 base;
        u64 length;
        u32 type;
        u32 reserved;
    };

    static_assert(std::is_standard_layout_v<ContainerHeader>);
    static_assert(std::is_trivially_copyable_v<ContainerHeader>);
    static_assert(sizeof(ContainerHeader) == 88);
    static_assert(offsetof(ContainerHeader, magic) == 0);
    static_assert(offsetof(ContainerHeader, reserved0) == 4);
    static_assert(offsetof(ContainerHeader, major) == 8);
    static_assert(offsetof(ContainerHeader, minor) == 10);
    static_assert(offsetof(ContainerHeader, header_size) == 12);
    static_assert(offsetof(ContainerHeader, container_size) == 16);
    static_assert(offsetof(ContainerHeader, item_table_offset) == 24);
    static_assert(offsetof(ContainerHeader, item_count) == 32);
    static_assert(offsetof(ContainerHeader, string_table_offset) == 36);
    static_assert(offsetof(ContainerHeader, string_table_size) == 40);
    static_assert(offsetof(ContainerHeader, setup_item_index) == 44);
    static_assert(offsetof(ContainerHeader, flags) == 48);
    static_assert(offsetof(ContainerHeader, container_digest) == 52);
    static_assert(offsetof(ContainerHeader, reserved) == 84);
    static_assert(std::is_standard_layout_v<ItemDescriptor>);
    static_assert(std::is_trivially_copyable_v<ItemDescriptor>);
    static_assert(sizeof(ItemDescriptor) == 96);
    static_assert(offsetof(ItemDescriptor, name_offset) == 0);
    static_assert(offsetof(ItemDescriptor, name_size) == 4);
    static_assert(offsetof(ItemDescriptor, reserved_name) == 6);
    static_assert(offsetof(ItemDescriptor, type) == 8);
    static_assert(offsetof(ItemDescriptor, flags) == 12);
    static_assert(offsetof(ItemDescriptor, format) == 16);
    static_assert(offsetof(ItemDescriptor, compression) == 20);
    static_assert(offsetof(ItemDescriptor, reserved) == 24);
    static_assert(offsetof(ItemDescriptor, payload_offset) == 32);
    static_assert(offsetof(ItemDescriptor, payload_size) == 40);
    static_assert(offsetof(ItemDescriptor, memory_size) == 48);
    static_assert(offsetof(ItemDescriptor, alignment) == 56);
    static_assert(offsetof(ItemDescriptor, digest) == 64);
    static_assert(std::is_standard_layout_v<MemoryRange>);
    static_assert(std::is_trivially_copyable_v<MemoryRange>);
    static_assert(sizeof(MemoryRange) == 24);
    static_assert(offsetof(MemoryRange, base) == 0);
    static_assert(offsetof(MemoryRange, length) == 8);
    static_assert(offsetof(MemoryRange, type) == 16);
    static_assert(offsetof(MemoryRange, reserved) == 20);
} // namespace obi

#endif // OBI_PACKAGE_FORMAT_HPP
