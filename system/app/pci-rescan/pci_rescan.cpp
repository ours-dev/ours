/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// pci-rescan -- asks a resident driver to look at the bus again (the PCI
/// hot-plug round): a device the machine was booted without is sized,
/// assigned, brought up and published by the driver the moment this asks.
///
///   pci-rescan nvme      the NVMe driver's rescan
///   pci-rescan virtio    the virtio-blk driver's rescan
///
/// It connects to /svc/ours.<driver>.Control in the namespace it was
/// started with -- the shell's, which the root offers both controls in --
/// and sends Control.Rescan. The reply's status is the answer:
///
///   Ok        a function was found and brought up; the device tree has a
///             new leaf, and the disk answers its block protocol;
///   NotFound  the walk saw nothing new -- an unchanged machine, an
///             answer and not a failure, so the exit is 0 either way;
///   anything else   why not, and the exit is 1.
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <string_view>

#include <fsio/fsio.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <prot/nvme_control.hpp>
#include <prot/nvme_control_client.hpp>
#include <prot/virtio_control.hpp>
#include <prot/virtio_control_client.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The rescan of one driver, named the way the command line names it: the
/// /svc path and the Client of that driver's control.
struct Driver {
    std::string_view name;
    std::string_view path;
    ours::Status (*rescan)(std::string_view path);
};

template <typename Client>
auto rescan_via(std::string_view const path) -> Status {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const made = ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        made != Status::Ok) {
        return made;
    }
    if (auto const connected = fsio::connect(path, server); connected != Status::Ok) {
        auto _ = ours::handle_close(client);
        return connected;
    }
    auto control = Client{client};
    return control.rescan();
}

auto rescan_nvme(std::string_view const path) -> Status {
    return rescan_via<prot::nvme::Client>(path);
}

auto rescan_virtio(std::string_view const path) -> Status {
    return rescan_via<prot::virtio::Client>(path);
}

constexpr auto DRIVERS = std::array{
    Driver{.name = "nvme", .path = prot::nvme::CONTROL_PATH, .rescan = rescan_nvme},
    Driver{.name = "virtio", .path = prot::virtio::CONTROL_PATH, .rescan = rescan_virtio},
};
} // namespace

auto main(int argc, char **argv) -> int {
    if (argc != 2) {
        auto _ = ort::debug_line("usage: pci-rescan nvme|virtio");
        return 2;
    }
    auto const asked = std::string_view{argv[1]};
    auto const driver = [&]() -> Driver const * {
        for (auto const &candidate: DRIVERS) {
            if (candidate.name == asked) {
                return &candidate;
            }
        }
        return nullptr;
    }();
    if (driver == nullptr) {
        auto _ = ort::debug_line({"pci-rescan: no such driver: ", asked});
        return 2;
    }

    auto const answer = driver->rescan(driver->path);
    if (answer == Status::Ok) {
        auto _ = ort::debug_line({"pci-rescan: ", driver->name, ": a device was claimed"});
        return 0;
    }
    if (answer == Status::NotFound) {
        auto _ = ort::debug_line({"pci-rescan: ", driver->name, ": nothing new on the bus"});
        return 0;
    }
    auto _ = ort::debug_line(
        {"pci-rescan: ", driver->name, ": the rescan answered ", ours::to_string(answer)});
    return 1;
}
