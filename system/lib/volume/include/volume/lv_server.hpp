/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef VOLUME_LV_SERVER_HPP
#define VOLUME_LV_SERVER_HPP 1

#include <volume/volume.hpp>

#include <array>
#include <span>

#include <block_server/block_server.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <prot/block.hpp>
#include <prot/block_client.hpp>

/// The logical volume as a block server: the same ours.block.Block the
/// device drivers speak, whose requests the mapping splits into runs and
/// issues one PV call per run. A filesystem on top cannot tell an LV from
/// a partition-sized disk -- the geometry is the LV's own, and every
/// sector goes where the metadata says.
///
/// The PVs are the caller's connections, one prot::block::Client per
/// index the mapping names; they are driven one call at a time (the tag
/// depth is 1, and the server runs on the Loop's thread), so the clients
/// serialize on this server the way the drivers' own do.
///
/// One copy of a Read's data: the runs are read into the server's own
/// buffer -- the mapping's runs are contiguous per PV, and the buffer is
/// the protocol's largest request -- and completed from there. A Write's
/// blocks go to the runs straight from the request, one slice each.
namespace volume {

/// How many PV clients one server drives: the mapping's PV indices name
/// them.
inline constexpr usize MAX_PVS = 8;

template <typename Self, usize MaxClients>
class LvBlockServer: public block_server::BlockServer<Self, MaxClients, 1> {
  public:
    using Base = block_server::BlockServer<Self, MaxClients, 1>;
    using Storage = typename Base::Storage;

    LvBlockServer(Storage &storage, loop::Loop &loop, VolumeGroup const &group,
                  std::span<prot::block::Client *const> const pvs)
        : Base(storage, loop), m_group(group), m_pv_count(pvs.size()) {
        for (auto index = usize{0}; index < pvs.size(); ++index) {
            m_pvs[index] = pvs[index];
        }
    }

    /// The LV's own geometry: the logical block is the 512-byte sector
    /// ours.block names, and the count is the LV's.
    [[nodiscard]]
    auto geometry() const -> block_server::Geometry {
        return {.block_size = prot::block::BLOCK_SIZE, .blocks = m_group.lv(m_lv).sector_count};
    }

    /// Which LV of the group this server answers as.
    auto set_lv(u16 const lv) -> void {
        m_lv = lv;
    }

    /// The library's hand-off: split the parked request into runs, issue
    /// one PV call per run, and complete. The runs of one request -- seven
    /// sectors at most, on at most seven PVs -- are issued in order and
    /// the whole request completes or fails together: a run that fails
    /// fails the request, and the runs already done are not undone, as
    /// any multi-disk write's partial failure is not.
    auto start(usize const index, std::span<u8 const> const data) -> ours::Status {
        auto const &parked = this->slot(index);
        auto runs = std::array<Run, prot::block::MAX_READ_BLOCKS>{};
        auto made = usize{0};
        auto const split = m_group.map_runs(m_lv, parked.lba, parked.count, runs, made);
        if (split != ours::Status::Ok) {
            return split;
        }
        if (data.empty()) {
            // A Read: the runs into the buffer, in order, and completed
            // from it.
            auto done = usize{0};
            for (auto const &run: std::span{runs}.first(made)) {
                auto const bytes = usize{run.count} * prot::block::BLOCK_SIZE;
                auto const into = std::span{m_buffer}.subspan(done, bytes);
                auto const status = m_pvs[run.pv]->read(run.first_sector, run.count, into);
                if (status != ours::Status::Ok) {
                    return status;
                }
                done += bytes;
            }
            Base::complete(index, ours::Status::Ok, std::span{m_buffer}.first(done));
            return ours::Status::Ok;
        }
        // A Write: the request's slices, one per run, straight through.
        auto done = usize{0};
        for (auto const &run: std::span{runs}.first(made)) {
            auto const bytes = usize{run.count} * prot::block::BLOCK_SIZE;
            auto const status = m_pvs[run.pv]->write(run.first_sector, data.subspan(done, bytes));
            if (status != ours::Status::Ok) {
                return status;
            }
            done += bytes;
        }
        Base::complete(index, ours::Status::Ok);
        return ours::Status::Ok;
    }

  private:
    VolumeGroup const &m_group;
    std::array<prot::block::Client *, MAX_PVS> m_pvs{};
    usize m_pv_count = 0;
    u16 m_lv = 0;
    std::array<u8, prot::block::MAX_READ_BLOCKS * prot::block::BLOCK_SIZE> m_buffer{};
};

} // namespace volume
#endif // #ifndef VOLUME_LV_SERVER_HPP
