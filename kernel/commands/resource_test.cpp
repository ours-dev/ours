/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `resource.test`: the ranged resource kinds and the ports no resource grants.
//
//   - ResourceDispatcher::create mints the root of Irq and of IoPort (base 0,
//     size 0) and ranges inside each kind's space, and refuses an empty
//     range that is not the root, a range past the space, the MmIo kind, a
//     number that names no kind and a System resource of size 0.
//   - A root grants its kind's whole space and nothing past it, and a range
//     grants only itself.
//   - Every port on the platform's list of the kernel's own
//     (platform::kernel_ioports()) is refused with AccessDenied, by the root
//     and by a range minted over it alike, its first and last port and any
//     request that overlaps it; the ports beside one are not.
//   - No interrupt line is refused that way: the kernel keeps its own lines
//     by being attached to them, which refuses any other attach, not through
//     the resource.
//   - A resource of one kind grants nothing of another.
//
// A console command because no system call hands validate_ranged_resource()
// an Irq or IoPort resource yet; until one does, this is how its answers for
// those kinds are seen.
#include <ours/object/resource_dispatcher.hpp>
#include <ours/platform/ioport.hpp>

#include <string_view>

#include <gktl/command.hpp>
#include <logz4/log.hpp>

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
        answered(tally, mint(&resource, RsrcKind::MmIo, 0, 0, "mmio"), Status::NotSupported,
                 "the MmIo kind is not supported");
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

    auto resource_test_command(int, char **) -> int {
        Tally tally;

        check_create(tally);
        check_ioport_root(tally);
        check_ioport_range(tally);
        check_irq_root(tally);

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
