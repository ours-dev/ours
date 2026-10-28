/// Host tests for bootshim::DevicetreeTranslator: the memory map, initrd and
/// command line a boot-shim takes from a flattened device tree.
///
/// Two trees come from QEMU's `virt` machine (-m 256M -smp 1, with a
/// 4096-byte -initrd):
/// - //kernel/phys/lib/fdt/test/data/qemu_virt.dtb, what QEMU hands the
///   firmware (`-machine virt,dumpdtb=...`);
/// - test/data/opensbi_virt.dtb, what the firmware (OpenSBI) hands the shim
///   in a1: saved from guest memory with the QEMU monitor's
///   `pmemsave 0x8fe00000 16384` while the shim was parked, and cut to the
///   blob's totalsize. OpenSBI adds /reserved-memory with the two regions its
///   PMP keeps from S-mode; /chosen/rng-seed is the only part that differs
///   from one boot to the next.
///
/// The rest are built by hand, one shape at a time. Every map is also fed to
/// bootmem::PhysicalMemory the way physload feeds it, to show physload takes
/// it without a refusal -- and so without a panic.

#include <bootshim/devicetree.hpp>

#include <array>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include <bootmem/physical_memory.hpp>
#include <fdt/tree.hpp>

namespace {
using bootshim::Conversion;
using bootshim::DevicetreeTranslator;
using bootshim::MAX_DEVICETREE_MEMORY_RANGES;
using bootshim::PhysRange;
using ours::Status;

constexpr auto PAGE = u64{0x1000};

auto require(bool const condition,
             std::source_location const where = std::source_location::current()) -> void {
    if (!condition) {
        std::fprintf(stderr, "devicetree_smoke: failed at line %u\n", where.line());
        std::abort();
    }
}

auto read_file(std::string_view const path) -> std::vector<u8> {
    std::ifstream file(std::string(path), std::ios::binary);
    require(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------------
// Building blobs
// ---------------------------------------------------------------------

auto append_be32(std::vector<u8> &bytes, u32 const value) -> void {
    bytes.push_back(static_cast<u8>(value >> 24));
    bytes.push_back(static_cast<u8>(value >> 16));
    bytes.push_back(static_cast<u8>(value >> 8));
    bytes.push_back(static_cast<u8>(value));
}

auto append_be64(std::vector<u8> &bytes, u64 const value) -> void {
    append_be32(bytes, static_cast<u32>(value >> 32));
    append_be32(bytes, static_cast<u32>(value));
}

auto put_be32(std::vector<u8> &bytes, usize const offset, u32 const value) -> void {
    bytes[offset] = static_cast<u8>(value >> 24);
    bytes[offset + 1] = static_cast<u8>(value >> 16);
    bytes[offset + 2] = static_cast<u8>(value >> 8);
    bytes[offset + 3] = static_cast<u8>(value);
}

/// Two cells per number, high cell first: a `reg` entry under 64-bit widths.
auto pair64(u64 const address, u64 const size) -> std::vector<u32> {
    return {static_cast<u32>(address >> 32), static_cast<u32>(address),
            static_cast<u32>(size >> 32), static_cast<u32>(size)};
}

/// A blob, written token by token. build() closes it with End.
class Blob final {
  public:
    auto begin(std::string_view const name) -> Blob & {
        token(fdt::Token::BeginNode);
        m_structure.insert(m_structure.end(), name.begin(), name.end());
        m_structure.push_back(0);
        pad();
        return *this;
    }

    auto end() -> Blob & {
        token(fdt::Token::EndNode);
        return *this;
    }

    auto prop(std::string_view const name, std::vector<u8> const &value) -> Blob & {
        token(fdt::Token::Prop);
        append_be32(m_structure, static_cast<u32>(value.size()));
        append_be32(m_structure, name_offset(name));
        m_structure.insert(m_structure.end(), value.begin(), value.end());
        pad();
        return *this;
    }

    auto prop_cells(std::string_view const name, std::vector<u32> const &cells) -> Blob & {
        std::vector<u8> bytes;
        for (auto const cell: cells) {
            append_be32(bytes, cell);
        }
        return prop(name, bytes);
    }

    auto prop_u32(std::string_view const name, u32 const value) -> Blob & {
        return prop_cells(name, {value});
    }

    auto prop_u64(std::string_view const name, u64 const value) -> Blob & {
        std::vector<u8> bytes;
        append_be64(bytes, value);
        return prop(name, bytes);
    }

    auto prop_string(std::string_view const name, std::string_view const value) -> Blob & {
        std::vector<u8> bytes(value.begin(), value.end());
        bytes.push_back(0);
        return prop(name, bytes);
    }

    /// An entry in the memory reservation block.
    auto reserve(u64 const address, u64 const size) -> Blob & {
        append_be64(m_reservations, address);
        append_be64(m_reservations, size);
        return *this;
    }

    [[nodiscard]]
    auto build() const -> std::vector<u8> {
        std::vector<u8> blob(fdt::HEADER_SIZE, 0);
        auto const reservation_offset = blob.size();
        blob.insert(blob.end(), m_reservations.begin(), m_reservations.end());
        append_be64(blob, 0);
        append_be64(blob, 0);

        auto const structure_offset = blob.size();
        blob.insert(blob.end(), m_structure.begin(), m_structure.end());
        append_be32(blob, static_cast<u32>(fdt::Token::End));
        auto const structure_size = blob.size() - structure_offset;

        auto const strings_offset = blob.size();
        blob.insert(blob.end(), m_strings.begin(), m_strings.end());

        put_be32(blob, fdt::header::MAGIC_OFFSET, fdt::MAGIC);
        put_be32(blob, fdt::header::TOTAL_SIZE, static_cast<u32>(blob.size()));
        put_be32(blob, fdt::header::STRUCTURE_OFFSET, static_cast<u32>(structure_offset));
        put_be32(blob, fdt::header::STRINGS_OFFSET, static_cast<u32>(strings_offset));
        put_be32(blob, fdt::header::RESERVATION_OFFSET, static_cast<u32>(reservation_offset));
        put_be32(blob, fdt::header::VERSION_OFFSET, fdt::VERSION);
        put_be32(blob, fdt::header::LAST_COMPATIBLE_VERSION, 16);
        put_be32(blob, fdt::header::STRINGS_SIZE, static_cast<u32>(m_strings.size()));
        put_be32(blob, fdt::header::STRUCTURE_SIZE, static_cast<u32>(structure_size));
        return blob;
    }

  private:
    auto token(fdt::Token const value) -> void {
        append_be32(m_structure, static_cast<u32>(value));
    }

    auto pad() -> void {
        while (m_structure.size() % fdt::TOKEN_ALIGN != 0) {
            m_structure.push_back(0);
        }
    }

    auto name_offset(std::string_view const name) -> u32 {
        auto const needle = std::string(name) + '\0';
        auto const found = m_strings.find(needle);
        if (found != std::string::npos) {
            return static_cast<u32>(found);
        }
        auto const offset = static_cast<u32>(m_strings.size());
        m_strings += needle;
        return offset;
    }

    std::vector<u8> m_structure;
    std::vector<u8> m_reservations;
    std::string m_strings;
};

constexpr auto RAM_BASE = u64{0x8000'0000};
constexpr auto RAM_SIZE = u64{0x1000'0000};

/// The root of a machine with 64-bit addresses and sizes, as QEMU's `virt`
/// writes it, left open for more nodes.
auto machine() -> Blob {
    Blob blob;
    blob.begin("").prop_u32("#address-cells", 2).prop_u32("#size-cells", 2);
    return blob;
}

auto memory_node(Blob &blob, std::string_view const name, std::vector<u32> const &reg) -> void {
    blob.begin(name).prop_string("device_type", "memory").prop_cells("reg", reg).end();
}

/// /reserved-memory with one child per entry of `regions`, using the given
/// cell widths of its own.
auto reserved_memory(Blob &blob, u32 const address_cells, u32 const size_cells,
                     std::vector<std::vector<u32>> const &regions) -> void {
    blob.begin("reserved-memory")
        .prop_u32("#address-cells", address_cells)
        .prop_u32("#size-cells", size_cells)
        .prop("ranges", {});
    for (usize index = 0; index < regions.size(); ++index) {
        auto const name = "region@" + std::to_string(index);
        blob.begin(name).prop_cells("reg", regions[index]).prop("no-map", {}).end();
    }
    blob.end();
}

auto chosen_initrd64(Blob &blob, u64 const start, u64 const end) -> void {
    blob.begin("chosen")
        .prop_u64("linux,initrd-start", start)
        .prop_u64("linux,initrd-end", end)
        .end();
}

// ---------------------------------------------------------------------
// Converting and checking
// ---------------------------------------------------------------------

auto parse(std::vector<u8> const &blob) -> fdt::Tree {
    auto const tree = fdt::Tree::from(blob.data(), blob.size());
    require(tree.is_ok());
    return tree.unwrap();
}

auto convert(DevicetreeTranslator &translator, std::vector<u8> const &blob,
             std::vector<PhysRange> const &reserved = {}, u64 const page_size = PAGE)
    -> ustl::Result<Conversion, Status> {
    return translator.convert(parse(blob), reserved, page_size);
}

auto convert_ok(DevicetreeTranslator &translator, std::vector<u8> const &blob,
                std::vector<PhysRange> const &reserved = {}) -> Conversion {
    auto const result = convert(translator, blob, reserved);
    require(result.is_ok());
    require(translator.ranges().size() == result.unwrap().converted);
    return result.unwrap();
}

auto convert_fails(std::vector<u8> const &blob, Status const status,
                   std::vector<PhysRange> const &reserved = {}, u64 const page_size = PAGE)
    -> void {
    DevicetreeTranslator translator;
    auto const result = convert(translator, blob, reserved, page_size);
    require(result.is_err());
    require(result.unwrap_err() == status);
    // A failed conversion leaves nothing behind for a careless caller to use.
    require(translator.ranges().empty());
    require(translator.initrd().is_none());
    require(translator.cmdline().empty());
}

auto is_range(obi::MemoryRange const &range, u64 const base, u64 const length,
              obi::MemoryRangeType const type) -> bool {
    return range.base == base && range.length == length && range.type == static_cast<u32>(type) &&
           range.reserved == 0;
}

auto is_ram(obi::MemoryRange const &range, u64 const base, u64 const length) -> bool {
    return is_range(range, base, length, obi::MemoryRangeType::Ram);
}

auto is_reserved(obi::MemoryRange const &range, u64 const base, u64 const length) -> bool {
    return is_range(range, base, length, obi::MemoryRangeType::Reserved);
}

/// physload's side of the map, step for step: register_range() for every range
/// (kernel/phys/memory.cpp) into a BootMem, then reserve_package() for the
/// initrd (BootPackageInput::reserve(): whole pages, as BootPackage). Every
/// step must succeed: physload panics on any refusal.
class Physload final {
  public:
    Physload() : m_memory({.usable = m_usable, .reservations = m_reservations}) {}

    auto take(DevicetreeTranslator const &translator) -> void {
        for (auto const &range: translator.ranges()) {
            auto const physical = bootmem::Range::from(range.base, range.length);
            require(physical.is_ok());
            if (range.type == static_cast<u32>(obi::MemoryRangeType::Ram)) {
                require(m_memory.add_usable(physical.unwrap(), 0) == Status::Ok);
            } else {
                require(range.type == static_cast<u32>(obi::MemoryRangeType::Reserved));
                require(m_memory.reserve(physical.unwrap(),
                                         bootmem::MemoryType::FirmwareReserved) == Status::Ok);
            }
        }
        if (translator.initrd().is_some()) {
            auto const initrd = translator.initrd().value();
            auto const size = (initrd.size + PAGE - 1) & ~(PAGE - 1);
            auto const package = bootmem::Range::from(initrd.base, size);
            require(package.is_ok());
            require(m_memory.reserve(package.unwrap(), bootmem::MemoryType::BootPackage) ==
                    Status::Ok);
        }
    }

    /// Whether every byte of [base, base + size) is usable RAM nobody holds.
    [[nodiscard]]
    auto is_free(u64 const base, u64 const size) const -> bool {
        return type_over(base, size, bootmem::MemoryType::Free);
    }

    /// Whether every byte of [base, base + size) is usable RAM held as `type`.
    [[nodiscard]]
    auto type_over(u64 const base, u64 const size, bootmem::MemoryType const type) const -> bool {
        std::array<bootmem::MemRange, 64> out{};
        auto const snapshot = m_memory.snapshot(out);
        require(snapshot.is_ok());
        for (auto const &entry: snapshot.unwrap()) {
            if (entry.range.base <= base && base + size <= entry.range.end()) {
                return entry.type == type;
            }
        }
        return false;
    }

    /// Whether any byte of [base, base + size) is usable RAM nobody holds.
    [[nodiscard]]
    auto any_free(u64 const base, u64 const size) const -> bool {
        std::array<bootmem::MemRange, 64> out{};
        auto const snapshot = m_memory.snapshot(out);
        require(snapshot.is_ok());
        for (auto const &entry: snapshot.unwrap()) {
            if (entry.type == bootmem::MemoryType::Free && entry.range.base < base + size &&
                base < entry.range.end()) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<bootmem::MemoryRegion, 100> m_usable{};
    std::array<bootmem::Reservation, 100> m_reservations{};
    bootmem::PhysicalMemory m_memory;
};

// ---------------------------------------------------------------------
// The two QEMU trees
// ---------------------------------------------------------------------

/// Where the shim reserves itself and the tree, in these tests.
constexpr auto SHIM = PhysRange{.base = 0x8020'0000, .size = 0x4290};
constexpr auto BLOB_BASE = u64{0x8fe0'0000};

auto qemu_dumped_tree() -> void {
    auto const blob = read_file(QEMU_VIRT_DTB);
    DevicetreeTranslator translator;
    auto const conversion =
        convert_ok(translator, blob, {SHIM, {.base = BLOB_BASE, .size = blob.size()}});
    require(conversion.converted == 3 && conversion.dropped == 0);

    // What QEMU hands the firmware has no /reserved-memory and an empty
    // reservation block: RAM, then the caller's two ranges.
    auto const ranges = translator.ranges();
    require(is_ram(ranges[0], RAM_BASE, RAM_SIZE));
    require(is_reserved(ranges[1], SHIM.base, SHIM.size));
    require(is_reserved(ranges[2], BLOB_BASE, blob.size()));

    require(translator.initrd().is_some());
    require(translator.initrd().value().base == 0x8820'0000);
    require(translator.initrd().value().size == 0x1000);
    require(translator.cmdline().empty());

    Physload physload;
    physload.take(translator);
}

auto opensbi_runtime_tree() -> void {
    auto const blob = read_file(OPENSBI_VIRT_DTB);
    DevicetreeTranslator translator;
    auto const conversion =
        convert_ok(translator, blob, {SHIM, {.base = BLOB_BASE, .size = blob.size()}});
    require(conversion.converted == 5 && conversion.dropped == 0);

    // The firmware's two regions, from /reserved-memory, between RAM and the
    // caller's ranges.
    auto const ranges = translator.ranges();
    require(is_ram(ranges[0], RAM_BASE, RAM_SIZE));
    require(is_reserved(ranges[1], 0x8000'0000, 0x4'0000));
    require(is_reserved(ranges[2], 0x8004'0000, 0x2'0000));
    require(is_reserved(ranges[3], SHIM.base, SHIM.size));
    require(is_reserved(ranges[4], BLOB_BASE, blob.size()));
    require(translator.initrd().is_some());
    require(translator.initrd().value().base == 0x8820'0000);
    require(translator.initrd().value().size == 0x1000);

    // Through physload's BootMem: the firmware's memory, the shim and the
    // tree are held, the initrd is the boot package's, and the RAM between
    // them is free.
    Physload physload;
    physload.take(translator);
    require(!physload.any_free(0x8000'0000, 0x6'0000));
    require(physload.type_over(0x8000'0000, 0x6'0000, bootmem::MemoryType::FirmwareReserved));
    require(physload.is_free(0x8006'0000, 0x1a'0000));
    require(physload.type_over(SHIM.base, SHIM.size, bootmem::MemoryType::FirmwareReserved));
    require(physload.type_over(BLOB_BASE, blob.size(), bootmem::MemoryType::FirmwareReserved));
    require(physload.type_over(0x8820'0000, 0x1000, bootmem::MemoryType::BootPackage));
}

// ---------------------------------------------------------------------
// Hand-built shapes
// ---------------------------------------------------------------------

auto overlapping_reservations() -> void {
    // Three regions that overlap: two firmware regions, and a reservation
    // block entry across both. All are listed; BootMem merges them, since they
    // are all one type.
    auto blob = machine();
    blob.reserve(0x8001'0000, 0x4'0000);
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    reserved_memory(blob, 2, 2, {pair64(0x8000'0000, 0x2'0000), pair64(0x8001'8000, 0x4'0000)});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 4);
    auto const ranges = translator.ranges();
    require(is_ram(ranges[0], RAM_BASE, RAM_SIZE));
    require(is_reserved(ranges[1], 0x8001'0000, 0x4'0000));
    require(is_reserved(ranges[2], 0x8000'0000, 0x2'0000));
    require(is_reserved(ranges[3], 0x8001'8000, 0x4'0000));

    Physload physload;
    physload.take(translator);
    require(!physload.any_free(0x8000'0000, 0x5'8000));
    require(physload.is_free(0x8005'8000, 0x1000));
}

auto reservation_at_ram_edges() -> void {
    // One region flush with RAM's first byte, one flush with its last.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    reserved_memory(blob, 2, 2,
                    {pair64(RAM_BASE, 0x1000), pair64(RAM_BASE + RAM_SIZE - 0x2000, 0x2000)});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 3);
    require(is_reserved(translator.ranges()[1], RAM_BASE, 0x1000));
    require(is_reserved(translator.ranges()[2], RAM_BASE + RAM_SIZE - 0x2000, 0x2000));

    Physload physload;
    physload.take(translator);
    require(!physload.any_free(RAM_BASE, 0x1000));
    require(physload.is_free(RAM_BASE + 0x1000, RAM_SIZE - 0x3000));
    require(!physload.any_free(RAM_BASE + RAM_SIZE - 0x2000, 0x2000));
}

auto reservation_outside_ram() -> void {
    // A region in no memory node -- below RAM, as a flash or ROM would be --
    // is listed all the same: BootMem keeps reservations apart from RAM, and
    // one outside RAM simply never covers anything usable.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    reserved_memory(blob, 2, 2, {pair64(0x2000'0000, 0x100'0000)});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 2);
    require(is_reserved(translator.ranges()[1], 0x2000'0000, 0x100'0000));

    Physload physload;
    physload.take(translator);
    require(physload.is_free(RAM_BASE, RAM_SIZE));
}

auto two_memory_nodes() -> void {
    // Two banks, the second with two `reg` entries. Not RAM: a node with a
    // `reg` and no device_type, one named like memory without device_type
    // "memory", one whose device_type is something else, and a memory-typed
    // node that is not a child of the root.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("flash@20000000").prop_cells("reg", pair64(0x2000'0000, 0x200'0000)).end();
    blob.begin("memory@40000000").prop_cells("reg", pair64(0x4000'0000, 0x1000)).end();
    blob.begin("pci@30000000")
        .prop_string("device_type", "pci")
        .prop_cells("reg", pair64(0x3000'0000, 0x1000'0000))
        .end();
    blob.begin("soc").prop_u32("#address-cells", 2).prop_u32("#size-cells", 2);
    memory_node(blob, "memory@50000000", pair64(0x5000'0000, 0x1000));
    blob.end();
    blob.begin("cpus").prop_u32("#address-cells", 1).prop_u32("#size-cells", 0);
    blob.begin("cpu@0").prop_string("device_type", "cpu").prop_cells("reg", {0}).end();
    blob.end();
    auto second = pair64(0x1'0000'0000, 0x4000'0000);
    for (auto const cell: pair64(0x2'0000'0000, 0x1000'0000)) {
        second.push_back(cell);
    }
    memory_node(blob, "memory@100000000", second);
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 3);
    require(is_ram(translator.ranges()[0], RAM_BASE, RAM_SIZE));
    require(is_ram(translator.ranges()[1], 0x1'0000'0000, 0x4000'0000));
    require(is_ram(translator.ranges()[2], 0x2'0000'0000, 0x1000'0000));

    Physload physload;
    physload.take(translator);
    require(physload.is_free(0x1'0000'0000, 0x4000'0000));
}

auto memory_reg_uses_the_roots_cell_widths() -> void {
    // 32-bit addresses and sizes at the root: each `reg` entry is two cells.
    Blob blob;
    blob.begin("").prop_u32("#address-cells", 1).prop_u32("#size-cells", 1);
    memory_node(blob, "memory@80000000", {0x8000'0000, 0x400'0000, 0xc000'0000, 0x100'0000});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 2);
    require(is_ram(translator.ranges()[0], 0x8000'0000, 0x400'0000));
    require(is_ram(translator.ranges()[1], 0xc000'0000, 0x100'0000));
}

auto reserved_memory_uses_its_own_cell_widths() -> void {
    // The root says two cells each; /reserved-memory says one. Read with the
    // root's widths, the two one-cell pairs below would be one wrong entry.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    reserved_memory(blob, 1, 1, {{0x8000'0000, 0x4'0000}, {0x8004'0000, 0x2'0000}});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 3);
    require(is_reserved(translator.ranges()[1], 0x8000'0000, 0x4'0000));
    require(is_reserved(translator.ranges()[2], 0x8004'0000, 0x2'0000));
}

auto reserved_memory_child_without_reg() -> void {
    // A child that asks to be allocated (`size`, no `reg`) names no memory.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("reserved-memory").prop_u32("#address-cells", 2).prop_u32("#size-cells", 2);
    blob.begin("pool").prop_u64("size", 0x10'0000).prop("reusable", {}).end();
    blob.end();
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 1);
}

auto no_reserved_memory() -> void {
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes, {SHIM}).converted == 2);
    require(is_ram(translator.ranges()[0], RAM_BASE, RAM_SIZE));
    require(is_reserved(translator.ranges()[1], SHIM.base, SHIM.size));
    require(translator.initrd().is_none());
    require(translator.cmdline().empty());
}

auto no_initrd() -> void {
    // /chosen without an initrd: none, and a reservation block entry is kept
    // whole.
    auto blob = machine();
    blob.reserve(0x8820'0000, 0x1000);
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("chosen").prop_string("stdout-path", "/soc/serial@10000000").end();
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 2);
    require(translator.initrd().is_none());
    require(is_reserved(translator.ranges()[1], 0x8820'0000, 0x1000));
}

auto initrd_in_32_bit_cells() -> void {
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("chosen")
        .prop_u32("linux,initrd-start", 0x8820'0000)
        .prop_u32("linux,initrd-end", 0x8820'3000)
        .end();
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 1);
    require(translator.initrd().is_some());
    require(translator.initrd().value().base == 0x8820'0000);
    require(translator.initrd().value().size == 0x3000);
}

auto initrd_in_mixed_cells() -> void {
    // Each bound is read on its own: one cell for the start, two for the end.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("chosen")
        .prop_u32("linux,initrd-start", 0x8820'0000)
        .prop_u64("linux,initrd-end", 0x8820'1800)
        .end();
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 1);
    require(translator.initrd().value().base == 0x8820'0000);
    require(translator.initrd().value().size == 0x1800);
}

auto empty_initrd_is_none() -> void {
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    chosen_initrd64(blob, 0x8820'0000, 0x8820'0000);
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 1);
    require(translator.initrd().is_none());
}

auto bootargs_is_the_command_line() -> void {
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    blob.begin("chosen").prop_string("bootargs", "kernel.phys.next=physboot aslr.disabled").end();
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 1);
    require(translator.cmdline() == "kernel.phys.next=physboot aslr.disabled");
}

auto reservation_block_is_cut_around_the_initrd() -> void {
    // A bootloader lists the initrd it loaded in the reservation block. The
    // initrd is 0x1800 bytes, so physload holds two pages for it, and the
    // entries are cut around both pages:
    // - one exactly the initrd's bytes leaves nothing (its tail page too);
    // - one spanning it from below and above leaves the part below the first
    //   page and the part above the last;
    // - one unrelated is kept whole.
    auto blob = machine();
    blob.reserve(0x8820'0000, 0x1800);
    blob.reserve(0x881f'0000, 0x2'0000);
    blob.reserve(0x8100'0000, 0x1000);
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    chosen_initrd64(blob, 0x8820'0000, 0x8820'1800);
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes).converted == 4);

    // physload first: an entry left over the initrd's pages fails here, at
    // its own reservation of the boot package, as it would panic in physload.
    Physload physload;
    physload.take(translator);
    require(physload.type_over(0x8820'0000, 0x2000, bootmem::MemoryType::BootPackage));

    auto const ranges = translator.ranges();
    require(is_ram(ranges[0], RAM_BASE, RAM_SIZE));
    require(is_reserved(ranges[1], 0x881f'0000, 0x1'0000));
    require(is_reserved(ranges[2], 0x8820'2000, 0xe000));
    require(is_reserved(ranges[3], 0x8100'0000, 0x1000));
}

auto firmware_region_over_the_initrd_is_refused() -> void {
    // The bootloader put the initrd where the firmware keeps memory for
    // itself: two owners for one page.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    reserved_memory(blob, 2, 2, {pair64(0x8820'1000, 0x1000)});
    chosen_initrd64(blob, 0x8820'0000, 0x8820'1800);
    convert_fails(blob.end().build(), Status::AlreadyExists);
}

auto callers_range_over_the_initrd_is_refused() -> void {
    // The initrd overlaps the shim's own image.
    auto blob = machine();
    memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    chosen_initrd64(blob, 0x8020'4000, 0x8020'5000);
    convert_fails(blob.end().build(), Status::AlreadyExists, {SHIM});

    // A range that only shares the initrd's last, partial page is refused
    // too: physload holds that whole page.
    auto shared = machine();
    memory_node(shared, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    chosen_initrd64(shared, 0x8820'0000, 0x8820'0800);
    convert_fails(shared.end().build(), Status::AlreadyExists,
                  {{.base = 0x8820'0c00, .size = 0x400}});
}

auto empty_entries_are_left_out() -> void {
    auto blob = machine();
    blob.reserve(0x8100'0000, 0);
    memory_node(blob, "memory@80000000", [] {
        auto cells = pair64(RAM_BASE, RAM_SIZE);
        for (auto const cell: pair64(0x9000'0000, 0)) {
            cells.push_back(cell);
        }
        return cells;
    }());
    reserved_memory(blob, 2, 2, {pair64(0x8000'0000, 0)});
    auto const bytes = blob.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, bytes, {{.base = 0x8020'0000, .size = 0}}).converted == 1);
    require(is_ram(translator.ranges()[0], RAM_BASE, RAM_SIZE));
}

auto capacity_overflow_is_counted() -> void {
    // One memory node with exactly as many banks as there is room for, then
    // the same with five more.
    auto build = [](usize const banks) {
        auto blob = machine();
        std::vector<u32> cells;
        for (usize index = 0; index < banks; ++index) {
            for (auto const cell: pair64(0x1'0000'0000 * (index + 1), 0x1000)) {
                cells.push_back(cell);
            }
        }
        memory_node(blob, "memory@100000000", cells);
        return blob.end().build();
    };

    DevicetreeTranslator full;
    auto const exact = convert_ok(full, build(MAX_DEVICETREE_MEMORY_RANGES));
    require(exact.converted == MAX_DEVICETREE_MEMORY_RANGES && exact.dropped == 0);

    DevicetreeTranslator over;
    auto const extra = convert_ok(over, build(MAX_DEVICETREE_MEMORY_RANGES + 5), {SHIM});
    require(extra.converted == MAX_DEVICETREE_MEMORY_RANGES);
    // The five banks past the end and the caller's range: six described,
    // none written.
    require(extra.dropped == 6);
    require(is_ram(over.ranges()[MAX_DEVICETREE_MEMORY_RANGES - 1],
                   0x1'0000'0000 * MAX_DEVICETREE_MEMORY_RANGES, 0x1000));
}

auto reconverting_starts_over() -> void {
    auto first = machine();
    memory_node(first, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
    chosen_initrd64(first, 0x8820'0000, 0x8820'1000);
    auto const with_initrd = first.end().build();

    auto second = machine();
    memory_node(second, "memory@40000000", pair64(0x4000'0000, 0x1000));
    auto const without = second.end().build();

    DevicetreeTranslator translator;
    require(convert_ok(translator, with_initrd, {SHIM}).converted == 2);
    require(translator.initrd().is_some());
    require(convert_ok(translator, without).converted == 1);
    require(is_ram(translator.ranges()[0], 0x4000'0000, 0x1000));
    require(translator.initrd().is_none());
}

auto malformed_trees_are_refused() -> void {
    auto with_memory = [] {
        auto blob = machine();
        memory_node(blob, "memory@80000000", pair64(RAM_BASE, RAM_SIZE));
        return blob;
    };

    // No RAM at all.
    {
        auto blob = machine();
        convert_fails(blob.end().build(), Status::NotFound);
    }
    // A `reg` that is not a whole number of entries.
    {
        auto blob = machine();
        memory_node(blob, "memory@80000000", {0, 0x8000'0000, 0, 0x1000'0000, 0});
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // A /reserved-memory region with a broken `reg`.
    {
        auto blob = with_memory();
        reserved_memory(blob, 2, 2, {{0, 0x8000'0000, 0}});
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // Three-cell addresses.
    {
        Blob blob;
        blob.begin("").prop_u32("#address-cells", 3).prop_u32("#size-cells", 2);
        memory_node(blob, "memory@80000000", {0, 0, 0x8000'0000, 0, 0x1000'0000});
        convert_fails(blob.end().build(), Status::NotSupported);
    }
    // A range that runs past 2^64, in each place a range comes from.
    {
        auto blob = machine();
        memory_node(blob, "memory@80000000", pair64(0xffff'ffff'ffff'f000, 0x1000));
        convert_fails(blob.end().build(), Status::OutOfRange);
    }
    {
        auto blob = with_memory();
        blob.reserve(0xffff'ffff'ffff'f000, 0x2000);
        convert_fails(blob.end().build(), Status::OutOfRange);
    }
    {
        auto blob = with_memory();
        reserved_memory(blob, 2, 2, {pair64(0xffff'ffff'ffff'0000, 0x1'0000)});
        convert_fails(blob.end().build(), Status::OutOfRange);
    }
    {
        convert_fails(with_memory().end().build(), Status::OutOfRange,
                      {{.base = 0xffff'ffff'ffff'ff00, .size = 0x100}});
    }
    // An initrd whose last page runs past 2^64.
    {
        auto blob = with_memory();
        chosen_initrd64(blob, 0xffff'ffff'ffff'e000, 0xffff'ffff'ffff'f001);
        convert_fails(blob.end().build(), Status::OutOfRange);
    }
    // One initrd bound without the other.
    {
        auto blob = with_memory();
        blob.begin("chosen").prop_u64("linux,initrd-start", 0x8820'0000).end();
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // An end below the start.
    {
        auto blob = with_memory();
        chosen_initrd64(blob, 0x8820'1000, 0x8820'0000);
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // A bound that is neither one cell nor two.
    {
        auto blob = with_memory();
        blob.begin("chosen")
            .prop("linux,initrd-start", {0x88, 0x20, 0x00})
            .prop_u32("linux,initrd-end", 0x8820'1000)
            .end();
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // A `bootargs` that is not one string.
    {
        auto blob = with_memory();
        blob.begin("chosen").prop("bootargs", {'a', 0, 'b', 0}).end();
        convert_fails(blob.end().build(), Status::InvArgs);
    }
    // A page size that is not a power of two.
    {
        convert_fails(with_memory().end().build(), Status::InvArgs, {}, 0x1800);
        convert_fails(with_memory().end().build(), Status::InvArgs, {}, 0);
    }
}

} // namespace

auto main() -> int {
    qemu_dumped_tree();
    opensbi_runtime_tree();

    overlapping_reservations();
    reservation_at_ram_edges();
    reservation_outside_ram();
    two_memory_nodes();
    memory_reg_uses_the_roots_cell_widths();
    reserved_memory_uses_its_own_cell_widths();
    reserved_memory_child_without_reg();
    no_reserved_memory();
    no_initrd();
    initrd_in_32_bit_cells();
    initrd_in_mixed_cells();
    empty_initrd_is_none();
    bootargs_is_the_command_line();
    reservation_block_is_cut_around_the_initrd();
    firmware_region_over_the_initrd_is_refused();
    callers_range_over_the_initrd_is_refused();
    empty_entries_are_left_out();
    capacity_overflow_is_counted();
    reconverting_starts_over();
    malformed_trees_are_refused();
    return 0;
}
