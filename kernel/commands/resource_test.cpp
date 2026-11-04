/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `resource.test`: the ranged resource kinds and what no resource grants.
//
//   - ResourceDispatcher::create mints the root of Irq, of IoPort and of MmIo
//     (base 0, size 0) and ranges inside each kind's space, and refuses an
//     empty range that is not the root, a range past the space, a number that
//     names no kind and a System resource of size 0.
//   - A root grants its kind's whole space and nothing past it, and a range
//     grants only itself.
//   - Every port on the platform's list of the kernel's own
//     (platform::kernel_ioports()) is refused with AccessDenied, by the root
//     and by a range minted over it alike, its first and last port and any
//     request that overlaps it; the ports beside one are not.
//   - The same for physical addresses: every run of RAM the kernel copied from
//     the memory manager's map (platform::kernel_ram()) and every device the
//     platform reserved (platform::kernel_mmio()) is refused with
//     AccessDenied, by the root and by a range minted over it, and a frame the
//     page allocator hands out is refused too. At every edge of every run the
//     root answers as a byte-by-byte model does, a page above all of them is
//     granted, and neither table lives in the init sections, whose frames the
//     page allocator owns once boot is over.
//   - No interrupt line is refused that way: the kernel keeps its own lines
//     by being attached to them, which refuses any other attach, not through
//     the resource.
//   - A resource of one kind grants nothing of another.
//
// A console command because no system call hands validate_ranged_resource()
// an Irq, IoPort or MmIo resource yet; until one does, this is how its answers
// for those kinds are seen.
#include <ours/mm/cfg.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/platform/ioport.hpp>
#include <ours/platform/mmio.hpp>

#include <algorithm>
#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours {
namespace {
    using object::KernelHandle;
    using object::ResourceDispatcher;
    using object::validate_ranged_resource;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("resource.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("resource.test: FAIL  {}", name);
    }

    /// Whether |got| is |want|, logging both when it is not.
    auto answered(Tally &tally, Status const got, Status const want, char const *const name)
        -> void {
        if (got != want) {
            log::error("resource.test:   {} answered {}, expected {}", name, to_string(got),
                       to_string(want));
        }
        verdict(tally, got == want, name);
    }

    auto mint(KernelHandle<ResourceDispatcher> *const out, RsrcKind const kind, u64 const base,
              u64 const size, char const *const name) -> Status {
        auto rights = Rights::None;
        return ResourceDispatcher::create(out, rights, kind, base, size, RsrcFlag{},
                                          ResourceDispatcher::Name(name));
    }

    auto check_create(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> resource;
        answered(tally, mint(&resource, RsrcKind::IoPort, 5, 0, "empty"), Status::InvArgs,
                 "an empty I/O port range that is not the root is refused");
        answered(tally, mint(&resource, RsrcKind::IoPort, 0, 0x10001, "wide"), Status::InvArgs,
                 "an I/O port range wider than the port space is refused");
        answered(tally, mint(&resource, RsrcKind::IoPort, 0xFFFF, 2, "past"), Status::InvArgs,
                 "an I/O port range running past 0xFFFF is refused");
        answered(tally, mint(&resource, RsrcKind::Irq, 0xFFFF'FFFF, 2, "past"), Status::InvArgs,
                 "an interrupt line range running past the u32 space is refused");
        answered(tally, mint(&resource, RsrcKind::MmIo, 5, 0, "empty"), Status::InvArgs,
                 "an empty physical range that is not the root is refused");
        answered(tally, mint(&resource, RsrcKind::MmIo, ~u64{0}, 2, "past"), Status::InvArgs,
                 "a physical range running past the 64-bit space is refused");
        answered(tally, mint(&resource, RsrcKind::MmIo, 0x1000, 0x1000, "page"), Status::Ok,
                 "a page is a physical range");
        // 3 is the number of the root kind the ABI retired.
        answered(tally, mint(&resource, static_cast<RsrcKind>(3), 0, 0, "retired"), Status::InvArgs,
                 "a number that names no kind is refused");
        answered(tally, mint(&resource, RsrcKind::System, 0, 0, "system"), Status::InvArgs,
                 "a System resource has no root");
        answered(tally, mint(&resource, RsrcKind::IoPort, 0xFFFF, 1, "last"), Status::Ok,
                 "the last I/O port alone is a range");
    }

    auto check_ioport_root(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> root;
        answered(tally, mint(&root, RsrcKind::IoPort, 0, 0, "ioport"), Status::Ok,
                 "mint the I/O port root");
        if (!root.dispatcher()) {
            return;
        }
        auto const &resource = *root.dispatcher();
        auto const info = resource.get_info();
        verdict(tally,
                resource.is_root() && info.kind == RsrcKind::IoPort && info.base == 0 &&
                    info.size == 0 && std::string_view(info.name) == "ioport",
                "the I/O port root reports itself as one");

        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x70, 2), Status::Ok,
                 "the root grants the RTC's ports");
        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0xFFFF, 1), Status::Ok,
                 "the root grants the last port");
        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0xFFFF, 2),
                 Status::OutOfRange, "the root grants nothing past 0xFFFF");
        answered(tally, validate_ranged_resource(resource, RsrcKind::Irq, 3, 1), Status::BadType,
                 "the I/O port root grants no interrupt line");

        auto denied = usize{0};
        auto granted = usize{0};
        for (auto const &range: platform::kernel_ioports()) {
            auto const first = u64{range.base};
            auto const last = first + range.count - 1;
            auto const refused = [&](u64 const base, u64 const size) {
                auto const status =
                    validate_ranged_resource(resource, RsrcKind::IoPort, base, size);
                if (status != Status::AccessDenied) {
                    log::error("resource.test:   [{:#x}, {:#x}) answered {}", base, base + size,
                               to_string(status));
                    return false;
                }
                return true;
            };
            // The first and last port, a request ending on the first and one
            // starting on the last.
            if (refused(first, 1) && refused(last, 1) && (first == 0 || refused(first - 1, 2)) &&
                refused(last, 2)) {
                denied += 1;
            }
            // The ports on either side, unless another of the kernel's own
            // ranges begins or ends there.
            auto const kernel_owned = [](u64 const port) {
                for (auto const &other: platform::kernel_ioports()) {
                    if (port >= other.base && port < u64{other.base} + other.count) {
                        return true;
                    }
                }
                return false;
            };
            auto const below_ok =
                first == 0 || kernel_owned(first - 1) ||
                validate_ranged_resource(resource, RsrcKind::IoPort, first - 1, 1) == Status::Ok;
            auto const above_ok =
                last == 0xFFFF || kernel_owned(last + 1) ||
                validate_ranged_resource(resource, RsrcKind::IoPort, last + 1, 1) == Status::Ok;
            if (below_ok && above_ok) {
                granted += 1;
            }
        }
        auto const ranges = platform::kernel_ioports().size();
        log::info("resource.test:   {} of the kernel's port ranges", ranges);
        verdict(tally, ranges != 0, "the platform names ports of its own");
        verdict(tally, denied == ranges, "the root grants none of the kernel's ports");
        verdict(tally, granted == ranges, "the root grants the ports beside them");
        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0, 0x10000),
                 Status::AccessDenied, "the root does not grant the whole port space at once");
    }

    auto check_ioport_range(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> rtc;
        answered(tally, mint(&rtc, RsrcKind::IoPort, 0x70, 2, "rtc"), Status::Ok,
                 "mint the RTC's two ports");
        if (rtc.dispatcher()) {
            auto const &resource = *rtc.dispatcher();
            verdict(tally, !resource.is_root(), "a range is not a root");
            answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x70, 2),
                     Status::Ok, "a range grants itself");
            answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x71, 1),
                     Status::Ok, "a range grants part of itself");
            answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x71, 2),
                     Status::OutOfRange, "a range grants nothing past its end");
            answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x6F, 1),
                     Status::OutOfRange, "a range grants nothing before its base");
        }

        // Minted over COM1, which create() allows: a range over the kernel's
        // ports grants none of them all the same.
        KernelHandle<ResourceDispatcher> com1;
        answered(tally, mint(&com1, RsrcKind::IoPort, 0x3F8, 8, "com1"), Status::Ok,
                 "mint a range over COM1");
        if (com1.dispatcher()) {
            answered(tally,
                     validate_ranged_resource(*com1.dispatcher(), RsrcKind::IoPort, 0x3F8, 1),
                     Status::AccessDenied, "a range over COM1 does not grant it");
        }
    }

    auto check_irq_root(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> root;
        answered(tally, mint(&root, RsrcKind::Irq, 0, 0, "irq"), Status::Ok,
                 "mint the interrupt line root");
        if (!root.dispatcher()) {
            return;
        }
        auto const &resource = *root.dispatcher();
        auto const info = resource.get_info();
        verdict(tally,
                resource.is_root() && info.kind == RsrcKind::Irq && info.base == 0 &&
                    info.size == 0 && std::string_view(info.name) == "irq",
                "the interrupt line root reports itself as one");
        answered(tally, validate_ranged_resource(resource, RsrcKind::Irq, 0, 1), Status::Ok,
                 "the root grants line 0, the kernel's or not");
        answered(tally, validate_ranged_resource(resource, RsrcKind::Irq, 0xFFFF'FFFF, 1),
                 Status::Ok, "the root grants the last line");
        answered(tally, validate_ranged_resource(resource, RsrcKind::Irq, 0xFFFF'FFFF, 2),
                 Status::OutOfRange, "the root grants nothing past the u32 space");
        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x70, 1),
                 Status::BadType, "the interrupt line root grants no port");
    }

    /// Whether |address| lies in one of |ranges|.
    auto lies_in(ustl::views::Span<platform::MmioRange const> const ranges, u64 const address)
        -> bool {
        return std::ranges::any_of(ranges, [&](platform::MmioRange const &range) {
            return address >= std::to_underlying(range.base) &&
                   address - std::to_underlying(range.base) < range.size;
        });
    }

    /// Whether the kernel owns the byte at |address|: touches_kernel_physical()
    /// worked out one byte at a time, to hold its answers against.
    auto owned(u64 const address) -> bool {
        return lies_in(platform::kernel_ram(), address) ||
               lies_in(platform::kernel_mmio(), address);
    }

    /// The first page above all the RAM and every device the kernel owns.
    auto page_above_everything() -> u64 {
        auto top = u64{0};
        for (auto const &range: platform::kernel_ram()) {
            top = std::max<u64>(top, std::to_underlying(range.base) + range.size);
        }
        for (auto const &range: platform::kernel_mmio()) {
            top = std::max<u64>(top, std::to_underlying(range.base) + range.size);
        }
        return ustl::round_up(top, u64{PAGE_SIZE});
    }

    /// Whether |resource| refuses with AccessDenied the four requests that pin
    /// down [base, base + size): its first byte, its last byte, one ending on
    /// the first and one starting on the last.
    auto refuses_around(ResourceDispatcher const &resource, u64 const base, u64 const size)
        -> bool {
        auto const refused = [&](u64 const first, u64 const length) {
            auto const status = validate_ranged_resource(resource, RsrcKind::MmIo, first, length);
            if (status != Status::AccessDenied) {
                log::error("resource.test:   [{:#x}, {:#x}) answered {}", first, first + length,
                           to_string(status));
                return false;
            }
            return true;
        };
        auto const last = base + size - 1;
        return refused(base, 1) && refused(last, 1) && (base == 0 || refused(base - 1, 2)) &&
               refused(last, 2);
    }

    /// Whether |resource| answers for the one byte at |address| as the model
    /// does: AccessDenied if the kernel owns it, Ok if not.
    auto agrees_at(ResourceDispatcher const &resource, u64 const address) -> bool {
        auto want = Status::Ok;
        if (owned(address)) {
            want = Status::AccessDenied;
        }
        auto const got = validate_ranged_resource(resource, RsrcKind::MmIo, address, 1);
        if (got != want) {
            log::error("resource.test:   byte {:#x} answered {}, expected {}", address,
                       to_string(got), to_string(want));
            return false;
        }
        return true;
    }

    /// The two tables the gate reads: how they are kept, and what is in them.
    auto check_mmio_tables(Tally &tally) -> void {
        auto const ram = platform::kernel_ram();
        auto const devices = platform::kernel_mmio();
        log::info("resource.test:   {} runs of RAM, {} devices of the kernel's own", ram.size(),
                  devices.size());
        for (auto const &range: devices) {
            log::info("resource.test:   device [{:#x}, {:#x})", std::to_underlying(range.base),
                      std::to_underlying(range.base) + range.size);
        }
        verdict(tally, !ram.empty(), "the kernel holds a copy of the RAM map");
        verdict(tally, !devices.empty(), "the platform names devices of its own");

        // Both are read long after boot, so neither may live in the init
        // sections: the page allocator owns those frames by then, and a new
        // owner's data would read as the map.
        auto const outside_init = [](void const *const pointer) {
            auto const address = reinterpret_cast<usize>(pointer);
            return address < reinterpret_cast<usize>(mm::KERNEL_INIT_START) ||
                   address >= reinterpret_cast<usize>(mm::KERNEL_INIT_END);
        };
        verdict(tally, outside_init(ram.data()) && outside_init(devices.data()),
                "the tables are outside the init sections");

        auto whole_pages = true;
        for (auto const &range: ram) {
            whole_pages = whole_pages && std::to_underlying(range.base) % PAGE_SIZE == 0 &&
                          range.size % PAGE_SIZE == 0;
        }
        for (auto const &range: devices) {
            whole_pages = whole_pages && std::to_underlying(range.base) % PAGE_SIZE == 0 &&
                          range.size % PAGE_SIZE == 0;
        }
        verdict(tally, whole_pages, "every run is whole pages");
    }

    auto check_mmio_root(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> root;
        answered(tally, mint(&root, RsrcKind::MmIo, 0, 0, "mmio"), Status::Ok,
                 "mint the MmIo root");
        if (!root.dispatcher()) {
            return;
        }
        auto const &resource = *root.dispatcher();
        auto const info = resource.get_info();
        verdict(tally,
                resource.is_root() && info.kind == RsrcKind::MmIo && info.base == 0 &&
                    info.size == 0 && std::string_view(info.name) == "mmio",
                "the MmIo root reports itself as one");

        auto const page = u64{PAGE_SIZE};
        auto const space = u64{1} << mm::get_phys_addr_bits();
        answered(tally,
                 validate_ranged_resource(resource, RsrcKind::MmIo, page_above_everything(), page),
                 Status::Ok,
                 "the root grants a page above all the memory and devices the kernel has");
        answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, space - page, page),
                 Status::Ok, "the root grants the last page of the space");
        answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, space - 1, 2),
                 Status::OutOfRange, "the root grants nothing past the physical address space");
        answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, ~u64{0}, 2),
                 Status::OutOfRange, "a request that wraps the 64-bit space is out of range");
        answered(tally, validate_ranged_resource(resource, RsrcKind::IoPort, 0x70, 1),
                 Status::BadType, "the MmIo root grants no port");

        auto ram_denied = usize{0};
        for (auto const &range: platform::kernel_ram()) {
            if (refuses_around(resource, std::to_underlying(range.base), range.size)) {
                ram_denied += 1;
            }
        }
        verdict(tally, ram_denied == platform::kernel_ram().size(),
                "the root grants none of the RAM");

        auto device_denied = usize{0};
        for (auto const &range: platform::kernel_mmio()) {
            if (refuses_around(resource, std::to_underlying(range.base), range.size)) {
                device_denied += 1;
            }
        }
        verdict(tally, device_denied == platform::kernel_mmio().size(),
                "the root grants none of those devices");

        // The model, at every edge of every run: the byte before it, its
        // first, its last and the byte after. An off-by-one anywhere shows up
        // as a byte the two disagree on.
        auto edges_agree = true;
        auto const probe = [&](platform::MmioRange const &range) {
            if (std::to_underlying(range.base) != 0) {
                edges_agree =
                    agrees_at(resource, std::to_underlying(range.base) - 1) && edges_agree;
            }
            edges_agree = agrees_at(resource, std::to_underlying(range.base)) && edges_agree;
            edges_agree =
                agrees_at(resource, std::to_underlying(range.base) + range.size - 1) && edges_agree;
            if (std::to_underlying(range.base) + range.size < space) {
                edges_agree =
                    agrees_at(resource, std::to_underlying(range.base) + range.size) && edges_agree;
            }
        };
        std::ranges::for_each(platform::kernel_ram(), probe);
        std::ranges::for_each(platform::kernel_mmio(), probe);
        verdict(tally, edges_agree, "the root answers at every edge as the model does");
        if (!platform::kernel_ram().empty()) {
            answered(tally,
                     validate_ranged_resource(resource, RsrcKind::MmIo,
                                              std::to_underlying(platform::kernel_ram()[0].base),
                                              0),
                     Status::Ok, "an empty request inside RAM grants nothing and touches nothing");
        }

        // A frame the page allocator has just handed out is RAM, and it is a
        // witness that does not come through the copy of the map.
        auto *const frame = mm::alloc_frames(mm::ALLOC_KERNEL, usize{0}).value_or(nullptr);
        if (frame == nullptr) {
            verdict(tally, false, "a frame to hold the gate against");
            return;
        }
        answered(tally,
                 validate_ranged_resource(resource, RsrcKind::MmIo,
                                          std::to_underlying(mm::frame_to_phys(frame)), page),
                 Status::AccessDenied, "the root refuses a frame the page allocator handed out");
        mm::free_frames(frame, 0);
    }

    auto check_mmio_range(Tally &tally) -> void {
        auto const page = u64{PAGE_SIZE};
        auto const hole = page_above_everything();

        KernelHandle<ResourceDispatcher> window;
        answered(tally, mint(&window, RsrcKind::MmIo, hole, 2 * page, "window"), Status::Ok,
                 "mint two pages above everything");
        if (window.dispatcher()) {
            auto const &resource = *window.dispatcher();
            verdict(tally, !resource.is_root(), "a physical range is not a root");
            answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, hole, 2 * page),
                     Status::Ok, "a range grants itself");
            answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, hole + page, page),
                     Status::Ok, "a range grants part of itself");
            answered(tally,
                     validate_ranged_resource(resource, RsrcKind::MmIo, hole + page, 2 * page),
                     Status::OutOfRange, "a range grants nothing past its end");
            answered(tally, validate_ranged_resource(resource, RsrcKind::MmIo, hole - page, page),
                     Status::OutOfRange, "a range grants nothing before its base");
        }

        // Minted over the first run of RAM, which create() allows: a range
        // over the kernel's memory grants none of it all the same.
        auto const ram = platform::kernel_ram();
        if (!ram.empty()) {
            KernelHandle<ResourceDispatcher> over_ram;
            answered(tally,
                     mint(&over_ram, RsrcKind::MmIo, std::to_underlying(ram[0].base), ram[0].size,
                          "ram"),
                     Status::Ok, "mint a range over RAM");
            if (over_ram.dispatcher()) {
                answered(tally,
                         validate_ranged_resource(*over_ram.dispatcher(), RsrcKind::MmIo,
                                                  std::to_underlying(ram[0].base), 1),
                         Status::AccessDenied, "a range over RAM does not grant it");
            }
        }

        // Minted over the first device the kernel drives, likewise.
        auto const devices = platform::kernel_mmio();
        if (!devices.empty()) {
            KernelHandle<ResourceDispatcher> over_device;
            answered(tally,
                     mint(&over_device, RsrcKind::MmIo, std::to_underlying(devices[0].base),
                          devices[0].size, "device"),
                     Status::Ok, "mint a range over a device of the kernel's");
            if (over_device.dispatcher()) {
                answered(tally,
                         validate_ranged_resource(*over_device.dispatcher(), RsrcKind::MmIo,
                                                  std::to_underlying(devices[0].base), 1),
                         Status::AccessDenied, "a range over a device does not grant it");
            }
        }
    }

    /// validate_root_resource(): the root of a kind is the one resource that
    /// passes, whatever the range a narrower one covers. interrupt_map asks it
    /// of the Irq resource, and a user program cannot make a narrower one, so
    /// this is where AccessDenied is shown.
    auto check_root_only(Tally &tally) -> void {
        KernelHandle<ResourceDispatcher> irq_root;
        KernelHandle<ResourceDispatcher> irq_one;
        KernelHandle<ResourceDispatcher> irq_all_but_last;
        KernelHandle<ResourceDispatcher> ioport_root;
        answered(tally, mint(&irq_root, RsrcKind::Irq, 0, 0, "irq"), Status::Ok,
                 "mint the interrupt root");
        answered(tally, mint(&irq_one, RsrcKind::Irq, 3, 1, "one"), Status::Ok,
                 "mint one interrupt line");
        answered(tally, mint(&irq_all_but_last, RsrcKind::Irq, 0, 0xFFFF'FFFF, "most"), Status::Ok,
                 "mint every interrupt line but the last");
        KernelHandle<ResourceDispatcher> irq_every_line;
        answered(tally, mint(&ioport_root, RsrcKind::IoPort, 0, 0, "ioport"), Status::Ok,
                 "mint the I/O port root");
        answered(tally, mint(&irq_every_line, RsrcKind::Irq, 0, u64{1} << 32, "every"), Status::Ok,
                 "mint a range over every interrupt line");
        if (!irq_root.dispatcher() || !irq_every_line.dispatcher()) {
            return;
        }
        if (!irq_root.dispatcher() || !irq_one.dispatcher() || !irq_all_but_last.dispatcher() ||
            !ioport_root.dispatcher()) {
            return;
        }

        answered(tally, validate_root_resource(*irq_root.dispatcher(), RsrcKind::Irq), Status::Ok,
                 "the interrupt root is the root of the interrupt lines");
        answered(tally, validate_root_resource(*irq_one.dispatcher(), RsrcKind::Irq),
                 Status::AccessDenied, "one interrupt line is not the root");
        answered(tally, validate_root_resource(*irq_all_but_last.dispatcher(), RsrcKind::Irq),
                 Status::AccessDenied, "nor is every line but one: a range is not the root");
        answered(tally, validate_root_resource(*irq_every_line.dispatcher(), RsrcKind::Irq),
                 Status::AccessDenied,
                 "a range over every line is not the root, though it grants every line");
        answered(tally, validate_ranged_resource(*irq_every_line.dispatcher(), RsrcKind::Irq, 5, 1),
                 Status::Ok, "and it does grant a line");
        answered(tally, validate_root_resource(*ioport_root.dispatcher(), RsrcKind::Irq),
                 Status::BadType, "the I/O port root is no interrupt root");
        answered(tally, validate_root_resource(*irq_root.dispatcher(), RsrcKind::IoPort),
                 Status::BadType, "and the interrupt root is no I/O port root");
    }

    auto resource_test_command(int, char **) -> int {
        Tally tally;

        check_create(tally);
        check_ioport_root(tally);
        check_ioport_range(tally);
        check_irq_root(tally);
        check_root_only(tally);
        check_mmio_tables(tally);
        check_mmio_root(tally);
        check_mmio_range(tally);

        if (tally.failed != 0) {
            log::error("resource.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("resource.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(ResourceTest, "resource.test", ours::resource_test_command,
             "mint IRQ and I/O port resources and check which ranges they grant");
