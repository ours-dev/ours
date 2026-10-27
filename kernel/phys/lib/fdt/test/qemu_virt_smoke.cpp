// The device tree QEMU's `virt` machine builds, read through fdt::Tree.
//
// The fixture, test/data/qemu_virt.dtb, was dumped with
//
//   qemu-system-riscv64 -machine virt,dumpdtb=qemu_virt.dtb -m 256M -smp 1 \
//       -nographic -bios default -kernel <any ELF> -initrd <a 4096-byte file>
//
// It is what QEMU hands the firmware, so it has no /reserved-memory node: the
// firmware adds its own region before passing the tree on.

#include <fstream>
#include <iterator>
#include <source_location>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include <fdt/tree.hpp>

namespace {
using fdt::Tree;

auto require(bool const condition,
             std::source_location const where = std::source_location::current()) -> void {
    if (!condition) {
        std::fprintf(stderr, "qemu_virt_smoke: failed at line %u\n", where.line());
        std::abort();
    }
}

auto read_fixture() -> std::vector<u8> {
    constexpr auto PATH = std::string_view(FDT_QEMU_VIRT_DTB);
    std::ifstream file(std::string(PATH), std::ios::binary);
    require(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

auto main() -> int {
    auto const blob = read_fixture();
    auto const parsed = Tree::from(blob.data(), blob.size());
    require(parsed.is_ok());
    auto const &tree = parsed.unwrap();
    require(tree.blob().size() == blob.size());
    require(tree.boot_cpu() == 0);
    require(tree.reservation_count() == 0);

    // The root: 64-bit addresses and sizes.
    auto const root = tree.root();
    require(root.address_cells().is_ok() && root.address_cells().unwrap() == 2);
    require(root.size_cells().is_ok() && root.size_cells().unwrap() == 2);

    // The one RAM node, and its reg decoded with the root's cell widths.
    auto const memory = tree.find("/memory");
    require(memory.is_some());
    require(memory.value().name() == "memory@80000000");
    require(memory.value().property("device_type").value().as_string().unwrap() == "memory");
    auto const range = memory.value().property("reg").value().reg(2, 2, 0);
    require(range.is_ok());
    require(range.unwrap().address == 0x8000'0000);
    require(range.unwrap().size == 0x1000'0000);
    require(memory.value().property("reg").value().reg(2, 2, 1).is_err());

    // The same node found by walking the root's children: exactly one is RAM.
    auto memory_nodes = usize{0};
    auto children = root.children();
    for (auto child = children.next(); child.is_some(); child = children.next()) {
        auto const type = child.value().property("device_type");
        if (type.is_some() && type.value().as_string().is_ok() &&
            type.value().as_string().unwrap() == "memory") {
            ++memory_nodes;
        }
    }
    require(memory_nodes == 1);

    // /chosen: where QEMU put the initrd, as 64-bit values, and the console.
    auto const chosen = tree.find("/chosen");
    require(chosen.is_some());
    require(chosen.value().property("linux,initrd-start").value().as_u64().unwrap() == 0x8820'0000);
    require(chosen.value().property("linux,initrd-end").value().as_u64().unwrap() == 0x8820'1000);
    require(chosen.value().property("stdout-path").value().as_string().unwrap() ==
            "/soc/serial@10000000");
    require(chosen.value().property("bootargs").is_none());

    // /cpus: one hart, whose ID is `reg`, read with the cpus node's own widths.
    auto const cpus = tree.find("/cpus");
    require(cpus.is_some());
    require(cpus.value().address_cells().unwrap() == 1);
    require(cpus.value().size_cells().unwrap() == 0);
    require(cpus.value().property("timebase-frequency").value().as_u32().unwrap() == 10'000'000);
    auto const hart = tree.find("/cpus/cpu@0");
    require(hart.is_some());
    require(hart.value().property("reg").value().reg(1, 0, 0).unwrap().address == 0);
    require(hart.value().property("riscv,isa-base").value().as_string().unwrap() == "rv64i");
    require(tree.find("/cpus/cpu@1").is_none());

    // The console device, three levels down.
    auto const serial = tree.find("/soc/serial");
    require(serial.is_some());
    require(serial.value().name() == "serial@10000000");
    require(serial.value().property("compatible").value().as_string().unwrap() == "ns16550a");
    auto const uart = serial.value().property("reg").value().reg(2, 2, 0);
    require(uart.unwrap().address == 0x1000'0000);
    require(uart.unwrap().size == 0x100);

    require(tree.find("/no-such-node").is_none());
    return 0;
}
