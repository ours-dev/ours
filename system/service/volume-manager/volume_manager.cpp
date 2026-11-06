/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// volume-manager -- the logical volume layer (the LVM round): it scans
/// the block devices its manifest routes to it for PV headers, assembles
/// the volume group, and serves the group's logical volumes as
/// ours.block.Block -- the same protocol the device drivers speak, so the
/// filesystem on top cannot tell an LV from a partition-sized disk, and
/// does not: the logical addressing is this component's.
///
/// The scan: each routed device is read at its first sectors, where a PV
/// header names itself -- magic, version, CRC, the VG it belongs to and
/// the segments this PV carries. A device without one is not ours; a
/// device with one joins the group. The group is complete when every
/// segment's PV joined and every striped LV's legs are all present; then
/// the served LV answers as a block device whose geometry is its own.
///
/// The objects:
///   - the PV clients, one per joined device, constructed in place on the
///     scan's ends and kept for the component's life (the process carries
///     no heap; the LV server drives them one call at a time);
///   - the group, assembled from the headers the scan found;
///   - the LV server, block_server's subclass over the group -- the
///     requests split into runs, one PV call per run;
///   - the Loop, the outgoing directory, and the one served protocol.
///
/// The events: the root routes the device drivers' ours.block.Block to
/// this component under the names its manifest uses; the scan reads each;
/// the complete group's first LV is served, and the filesystem routes to
/// it open the LV and never the disk.
///
/// Resident, as the drivers are: a scan that finds no complete group is
/// an answer, not an exit -- the component serves nothing and waits. One
/// group, its first LV served this round; multi-LV exposure is the
/// device tree's work (dev/class/block), a later round's.
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/time.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <span>
#include <string_view>

#include <ddk/ddk.hpp>
#include <fsio/fsio.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/spin_hint.hpp>
#include <ort/startup.hpp>
#include <prot/block.hpp>
#include <ustl/lazy_init.hpp>
#include <volume/lv_server.hpp>
#include <volume/volume.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The routes the scan reads PVs from: the root offers the device
/// drivers' ours.block.Block here under these names, one per driver that
/// carries a disk this machine may grow a volume on. A name the namespace
/// does not answer (no such use, or the driver went) is a device absent,
/// an answer and not a failure.
constexpr std::array PV_ROUTES = {std::string_view{"/svc/ours.block.Pv"},
                                  std::string_view{"/svc/ours.block.Pv2"}};

/// How many PVs this machine routes: one per route above, and the bound
/// the group's own mapping carries further.
constexpr auto MAX_PVS = PV_ROUTES.size();

/// The LV the component serves: the group's first. Multi-LV exposure is
/// the device tree's work, a later round's.
constexpr u16 SERVED_LV = 0;

/// Reads the device's first sectors and decodes a PV header: the primary
/// at LBA 0, the secondary at LBA HEADER_SECTORS -- either answering.
/// NotFound when the device names none, which is how a plain disk skips.
auto read_pv_header(prot::block::Client &client, volume::Header &header) -> Status {
    auto sectors = std::array<u8, 2 * volume::HEADER_SECTORS * volume::SECTOR_BYTES>{};
    if (auto const status = client.read(0, 2 * volume::HEADER_SECTORS, sectors);
        status != Status::Ok) {
        return status;
    }
    for (auto copy = usize{0}; copy < volume::HEADER_SECTORS; ++copy) {
        auto const at =
            std::span<u8 const>{sectors}.subspan(copy * volume::HEADER_BYTES, volume::HEADER_BYTES);
        auto const decoded = volume::decode_header(at);
        if (decoded.is_ok()) {
            header = decoded.unwrap();
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

/// The PV clients, constructed in place on the scan's ends: the process
/// carries no heap, and the clients' buffers live here for its life.
constinit std::array<ustl::LazyInit<prot::block::Client>, MAX_PVS> PV_CLIENTS{};

/// The scan's answer: the headers that joined -- in route order, each
/// naming the PV index its group gives it -- and the client ends they
/// were read through, still open; the component constructs its clients
/// on them.
struct Scan {
    std::array<volume::Header, MAX_PVS> headers{};
    std::array<prot::block::Client *, MAX_PVS> clients{};
    usize count = 0;
};

auto scan() -> Scan {
    auto scan = Scan{};
    for (auto const route: PV_ROUTES) {
        if (scan.count == MAX_PVS) {
            break;
        }
        auto client_end = Uandle::Invalid;
        auto server_end = Uandle::Invalid;
        if (auto const made =
                ours::channel_create(ours::ChannelCreateOptions::None, &client_end, &server_end);
            made != Status::Ok) {
            auto _ =
                ort::debug_line({"volume-manager: cannot make a channel: ", ours::to_string(made)});
            continue;
        }
        if (auto const connected = fsio::connect(route, server_end); connected != Status::Ok) {
            // The route leads nowhere: that device is absent.
            auto _ = ours::handle_close(client_end);
            continue;
        }
        // The probing client is the kept one: constructed in place, it
        // reads the header now and serves the LV server for the
        // component's life.
        auto &kept = *PV_CLIENTS[scan.count].init(client_end);
        auto header = volume::Header{};
        if (read_pv_header(kept, header) != Status::Ok) {
            // A plain disk, or a gone one: not ours, skipped -- the
            // client end goes with it.
            auto _ = ours::handle_close(client_end);
            continue;
        }
        scan.headers[scan.count] = header;
        scan.clients[scan.count] = &kept;
        scan.count += 1;
    }
    return scan;
}

constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + 2;

/// The CRTP self the server template asks for: the component's subclass
/// carries nothing of its own -- geometry and start are the library's.
struct Served final: volume::LvBlockServer<Served, 2> {
    using LvBlockServer::LvBlockServer;
};

constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<Served::Storage> SERVER_STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
/// The PV clients, constructed in place on the scan's ends: the process
/// carries no heap, and the clients' buffers live here for its life.

} // namespace

auto main(int, char **) -> int {
    auto const directory_request =
        ddk::startup_handle(ours::HandleType::DirectoryRequest, "a DirectoryRequest handle");
    if (directory_request.is_err()) {
        return 1;
    }

    // The scan races the drivers' own boot: the root starts its children
    // one after another, and a driver's block protocol appears in its
    // outgoing only once its first rescan has claimed the disk. The scan
    // is redone until the group completes or the seconds run out -- the
    // bounded wait the nvme driver spins on its controller with -- and
    // each round's failed ends close, so a retry starts clean.
    auto group = volume::VolumeGroup{};
    auto found = scan();
    auto const started = ours::clock_get_monotonic();
    for (;;) {
        for (auto index = usize{0}; index < found.count; ++index) {
            if (auto const added = group.add_pv(found.headers[index]); added != Status::Ok) {
                auto _ =
                    ort::debug_line({"volume-manager: a PV refused: ", ours::to_string(added)});
            }
        }
        if (group.is_complete()) {
            break;
        }
        auto const now = ours::clock_get_monotonic();
        if (now - started > 5'000'000) {
            break;
        }
        // The wait between rounds: a quarter-second of spins, wall-clock,
        // so the drivers behind the routes see a visitor and not a
        // hammer -- the devices answer at their own pace, and a resident
        // scanner that reads a device thousands of times a second is a
        // denial of service to whoever shares it.
        while (ours::clock_get_monotonic() - now < 250'000) {
            ort::spin_hint();
        }
        group = volume::VolumeGroup{};
        found = scan();
    }
    if (!group.is_complete()) {
        // No complete group: an answer, not a failure -- the routes to
        // this component's protocol answer NotFound while it is gone,
        // and a later round's rescan would look again.
        auto _ = ort::debug_line("volume-manager: no complete group; resident");
        auto _ = ours::handle_close(directory_request.unwrap());
        return 0;
    }

    // The machine: the port, the Loop, the outgoing, the LV server over
    // the group's first LV, served as ours.block.Block.
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ =
            ort::debug_line({"volume-manager: cannot make its port: ", ours::to_string(status)});
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    auto &outgoing_storage = *OUTGOING_STORAGE.init();
    ioservice::Outgoing outgoing(outgoing_storage, loop);
    auto &storage = *SERVER_STORAGE.init();

    // The PV clients, constructed on the scan's ends in the group's own
    // PV order: the mapping names them by index, and the server drives
    // them one call at a time.
    std::array<prot::block::Client *, MAX_PVS> pv_clients{};
    for (auto index = usize{0}; index < found.count; ++index) {
        pv_clients[found.headers[index].pv_index] = found.clients[index];
    }
    auto const pv_span = std::span<prot::block::Client *const>{pv_clients}.first(found.count);

    Served server(storage, loop, group, pv_span);
    server.set_lv(SERVED_LV);
    if (auto const status = outgoing.add_protocol(prot::block::BLOCK_NAME, server);
        status != Status::Ok) {
        auto _ = ort::debug_line(
            {"volume-manager: cannot serve its protocol: ", ours::to_string(status)});
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        auto _ = ort::debug_line(
            {"volume-manager: cannot serve its outgoing: ", ours::to_string(status)});
        return 1;
    }

    auto const &lv = group.lv(SERVED_LV);
    auto const name = std::string_view{reinterpret_cast<char const *>(lv.name.data())};
    auto _ = ort::debug_line({"volume-manager: serving LV ", name});

    auto _ = ort::debug_line(
        {"volume-manager: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
