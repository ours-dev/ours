/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The virtio library on two fakes. The probe runs over a fake ECAM array,
// as the pci and nvme suites' do. The bring-up and the submit path run
// against a fake device: a Ports implementation backed by arrays, whose
// reads answer and whose stores act -- writing the used ring's entry, or
// refusing a reset -- so the sequence the driver drives has the device's
// answers, as it does on silicon.
#include <virtio/virtio.hpp>

#include <array>
#include <span>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>

namespace {
using ours::PhysAddr;
using ours::Status;
using ours::VirtAddr;

constexpr auto PAGE = usize{4096};

/// The fake ECAM: the whole of bus 0, plus the sizing hardware's answer
/// sheet -- what each BAR register reads back when all ones are stored to
/// it (the probe write), one word per config word per function, as the pci
/// library's suite models it.
class FakeEcam {
  public:
    FakeEcam() {
        // A vacant PCI function reads all ones: the array starts filled.
        bytes_.fill(0xFF);
    }

    /// Lays function |index|'s page in: vendor, device, class code (stored
    /// shifted up past the revision byte, as config space packs it), and
    /// the BAR words the walk reports.
    auto place(usize const index, u16 const vendor, u16 const device, u32 const class_code,
               std::array<u32, 6> const &bars) -> void {
        word(index, 0, static_cast<u32>(vendor) | static_cast<u32>(device) << 16);
        word(index, 8, class_code << 8);
        for (auto i = usize{0}; i < 6; ++i) {
            word(index, 0x10 + i * 4, bars[i]);
        }
    }

    /// The sizing hardware's mask for function |index|'s BAR |bar|.
    auto set_mask(usize const index, usize const bar, u32 const mask) -> void {
        masks_[(0x10 + bar * 4) / 4 + index * (PAGE / 4)] = mask;
    }

    auto config() -> pci::Config {
        return pci::Config{std::span{bytes_}, masks_};
    }

    /// The raw word of function |index|'s register |offset|: what the
    /// assignment left behind.
    auto word(usize const index, usize const offset) const -> u32 {
        u32 value = 0;
        for (auto i = usize{0}; i < 4; ++i) {
            value |= static_cast<u32>(bytes_[index * PAGE + offset + i]) << (8 * i);
        }
        return value;
    }

  private:
    auto word(usize const index, usize const offset, u32 const value) -> void {
        for (auto i = usize{0}; i < 4; ++i) {
            bytes_[index * PAGE + offset + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }

    // The whole of bus 0: 256 function pages, the walk's full range.
    std::array<u8, PAGE * pci::FUNCTIONS_PER_BUS> bytes_{};
    std::array<u32, PAGE * pci::FUNCTIONS_PER_BUS / 4> masks_{};
};

/// One store the fake device saw, the fields its assertions name.
struct Recorded {
    u16 offset = 0;
    u32 value = 0;
    /// The access's width, in bytes.
    usize width = 0;
};

/// The fake device: the register ports and the region, whose reads answer
/// from the arrays and whose stores act. QUEUE_NUM answers 256 (the
/// device's maximum); the status register holds what was last written,
/// unless |stuck_reset_| keeps the reset write from taking; the device
/// config reads the capacity.
class FakeDevice final: public virtio::Ports {
  public:
    static constexpr auto REGION_PHYS = PhysAddr{0x2000'0000};

    [[nodiscard]]
    auto region_map() const -> VirtAddr {
        return VirtAddr{reinterpret_cast<usize>(region_.data())};
    }

    [[nodiscard]]
    auto region() const -> std::span<u8 const> {
        return region_;
    }

    /// The region, writable: the test's stand-in for the server placing a
    /// Write's blocks where the device reads them.
    auto region_bytes() -> std::span<u8> {
        return region_;
    }

    [[nodiscard]]
    auto stores() const -> std::vector<Recorded> const & {
        return stores_;
    }

    auto set_capacity(u64 const blocks) -> void {
        capacity_ = blocks;
    }
    auto stick_reset() -> void {
        stuck_reset_ = true;
        // In some state before the reset, which the reset write then never
        // leaves.
        status_ = 0xFF;
    }

    /// The device's own completion: the used ring's index advanced to the
    /// chains taken so far -- the notify counted this one's offer -- as
    /// the device writes it when the request is done.
    auto complete_used() -> void {
        auto *const used = region_.data() + virtio::USED_OFFSET;
        u16 const seen = static_cast<u16>(offered_);
        std::memcpy(used + 2, &seen, sizeof(seen));
    }

    /// The device's own answer to a read's chain: the block pattern it
    /// read into the data area, |blocks| of them, and the status byte
    /// after them -- where the chain's third descriptor names it.
    auto answer_read(usize const blocks, u8 const status) -> void {
        auto *const bytes = region_.data();
        for (auto i = usize{0}; i < blocks * virtio::SECTOR; ++i) {
            bytes[virtio::DATA_OFFSET + i] = static_cast<u8>(i % 251 + 1);
        }
        bytes[virtio::REQUEST_OFFSET + virtio::HEADER + blocks * virtio::SECTOR] = status;
    }

    /// The device's own answer to a write's chain: the data are the
    /// driver's own, and only the status byte lands, after them.
    auto answer_write(usize const blocks, u8 const status) -> void {
        region_[virtio::REQUEST_OFFSET + virtio::HEADER + blocks * virtio::SECTOR] = status;
    }

    auto in8(u16 const offset) const -> u8 override {
        if (offset == virtio::REG_STATUS) {
            return status_;
        }
        if (offset == virtio::REG_ISR) {
            return 1;
        }
        return 0;
    }

    auto in16(u16 const offset) const -> u16 override {
        if (offset == virtio::REG_QUEUE_NUM) {
            return 256;
        }
        return 0;
    }

    auto in32(u16 const offset) const -> u32 override {
        if (offset == virtio::REG_DEVICE_CONFIG) {
            return static_cast<u32>(capacity_);
        }
        if (offset == virtio::REG_DEVICE_CONFIG + 4) {
            return static_cast<u32>(capacity_ >> 32);
        }
        return 0;
    }

    auto out8(u16 const offset, u8 const value) const -> void override {
        if (offset == virtio::REG_STATUS) {
            if (value == 0 && stuck_reset_) {
                return; // the reset write never takes
            }
            status_ = value;
        }
        record(offset, value, 1);
    }

    auto out16(u16 const offset, u16 const value) const -> void override {
        if (offset == virtio::REG_QUEUE_NOTIFY) {
            // The notify is where the device would take the chain; the
            // completion the test drives with complete_used().
            offered_ += 1;
        }
        record(offset, value, 2);
    }

    auto out32(u16 const offset, u32 const value) const -> void override {
        record(offset, value, 4);
    }

  private:
    auto record(u16 const offset, u32 const value, usize const width) const -> void {
        stores_.push_back(Recorded{.offset = offset, .value = value, .width = width});
    }

    mutable std::array<u8, virtio::REGION_BYTES> region_{};
    mutable std::vector<Recorded> stores_;
    mutable u8 status_ = 0;
    mutable u64 offered_ = 0;
    u64 capacity_ = 64;
    bool stuck_reset_ = false;
};

/// The probe: the walk claims the vendor-0x1AF4 block function, keeps the
/// firmware's I/O base, and answers NotFound on a bus with none.
TEST(VirtioProbe, ClaimsTheVendorMatchAndKeepsItsBase) {
    auto ecam = FakeEcam{};
    ecam.place(3, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0xC001u, 0, 0, 0, 0, 0});
    auto arena = virtio::IoArena{0xE000, 0xFF00};
    auto config = ecam.config();
    auto const found = virtio::probe(config, arena, {});
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found->index, 3u);
    EXPECT_EQ(found->io_base, 0xC000u);
    // Nothing was assigned: the command register is untouched.
    EXPECT_EQ(ecam.word(3, 4), 0xFFFF'FFFFu);
}

/// The modern-only device id is driven by the same legacy interface.
TEST(VirtioProbe, ClaimsTheModernIdToo) {
    auto ecam = FakeEcam{};
    ecam.place(5, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_MODERN, 0x010000,
               {0xC041u, 0, 0, 0, 0, 0});
    auto arena = virtio::IoArena{0xE000, 0xFF00};
    auto config = ecam.config();
    auto const found = virtio::probe(config, arena, {});
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found->io_base, 0xC040u);
}

TEST(VirtioProbe, NotFoundOnAnEmptyBus) {
    auto ecam = FakeEcam{};
    auto arena = virtio::IoArena{0xE000, 0xFF00};
    auto config = ecam.config();
    EXPECT_EQ(virtio::probe(config, arena, {}).unwrap_err(), Status::NotFound);
}

/// The right vendor is not enough: another virtio device's function (a
/// network one) walks past, and so does an unassigned function whose I/O
/// BAR's probe answer names no decode.
TEST(VirtioProbe, SkipsOtherVirtioDevicesAndBlankBars) {
    auto ecam = FakeEcam{};
    // virtio-net, transitional: the vendor matches, the device id does not.
    ecam.place(2, virtio::VIRTIO_VENDOR, 0x1000, 0x010000, {0xC001u, 0, 0, 0, 0, 0});
    // The block device's id, but an I/O BAR with no base whose probe
    // answer is the flag bit alone: no decode to size, nothing to claim.
    ecam.place(4, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0x1u, 0, 0, 0, 0, 0});
    ecam.set_mask(4, 0, 0x1u);
    auto arena = virtio::IoArena{0xE000, 0xFF00};
    auto config = ecam.config();
    EXPECT_EQ(virtio::probe(config, arena, {}).unwrap_err(), Status::NotFound);
}

TEST(VirtioProbe, SkipsOwnedAndMemoryBars) {
    auto ecam = FakeEcam{};
    // A memory BAR0 (bit 0 clear) is not the legacy interface, even from
    // the right vendor.
    ecam.place(2, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0xE000'0000u, 0, 0, 0, 0, 0});
    ecam.place(7, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0xC081u, 0, 0, 0, 0, 0});
    auto arena = virtio::IoArena{0xE000, 0xFF00};

    std::array const owned{usize{7}};
    auto config = ecam.config();
    EXPECT_EQ(virtio::probe(config, arena, std::span<usize const>{owned}).unwrap_err(),
              Status::NotFound);
}

/// A function that arrived with no base -- a hot-plugged one, whose I/O
/// BAR reads its flag bit over a zero base -- is sized, assigned from the
/// port arena, and enabled.
TEST(VirtioProbe, AssignsTheUnassigned) {
    auto ecam = FakeEcam{};
    // The I/O flag over no base: bit 0 set, the base bits clear.
    ecam.place(5, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0x1u, 0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0xFFFF'FFF1u); // a 16-port decode
    auto arena = virtio::IoArena{0xE000, 0xFF00};

    auto config = ecam.config();
    auto const found = virtio::probe(config, arena, {});
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found->index, 5u);
    EXPECT_EQ(found->io_base, 0xE000u);
    // The assignment keeps the register's type bits (the I/O flag in bit
    // 0) and carries the base above them.
    EXPECT_EQ(ecam.word(5, 0x10), 0xE001u);
    // The command register carries io + memory + master: the function
    // decodes and masters.
    EXPECT_EQ(ecam.word(5, 4), 0x7u);
}

/// An arena with no room for the decode is the probe's failure, not a
/// silent overlap.
TEST(VirtioProbe, RefusesWhenTheArenaRunsOut) {
    auto ecam = FakeEcam{};
    ecam.place(5, virtio::VIRTIO_VENDOR, virtio::VIRTIO_BLK_TRANSITIONAL, 0x010000,
               {0x1u, 0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0xFFFF'FFF1u); // a 16-port decode
    auto arena = virtio::IoArena{0xFFF0, 0xFFFA};

    auto config = ecam.config();
    EXPECT_EQ(virtio::probe(config, arena, {}).unwrap_err(), Status::NoResource);
}

/// The port arena's own contract: runs aligned to their size, taken
/// upward, and refused past the limit or of no size at all.
TEST(VirtioIoArena, AlignsAndRefuses) {
    auto arena = virtio::IoArena{0xE004, 0xFF00};
    auto const first = arena.take(16);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first.unwrap(), 0xE010u);
    auto const second = arena.take(4);
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(second.unwrap(), 0xE020u);

    auto cramped = virtio::IoArena{0xFFF8, 0xFFFA};
    EXPECT_EQ(cramped.take(16).unwrap_err(), Status::NoResource);
    auto any = virtio::IoArena{0xE000, 0xFF00};
    EXPECT_EQ(any.take(0).unwrap_err(), Status::NoResource);
}

/// Bring-up: the status sequence in its recorded order, the queue's depth
/// and page named, and DRIVER_OK last.
TEST(VirtioDevice, BringsUp) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);

    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);

    auto const &stores = device_hw.stores();
    // The status writes: reset, acknowledge, driver, and the full set last.
    std::vector<u32> statuses;
    for (auto const &store: stores) {
        if (store.offset == virtio::REG_STATUS) {
            statuses.push_back(store.value);
        }
    }
    ASSERT_EQ(statuses.size(), 4u);
    EXPECT_EQ(statuses[0], 0u);
    EXPECT_EQ(statuses[1], static_cast<u32>(virtio::STATUS_ACKNOWLEDGE));
    EXPECT_EQ(statuses[2], static_cast<u32>(virtio::STATUS_ACKNOWLEDGE | virtio::STATUS_DRIVER));
    EXPECT_EQ(statuses[3], static_cast<u32>(virtio::STATUS_ACKNOWLEDGE | virtio::STATUS_DRIVER |
                                            virtio::STATUS_FEATURES_OK | virtio::STATUS_DRIVER_OK));
    // No features accepted; queue 0 selected; the driver's depth within
    // the device's maximum; the ring's page named.
    auto const find_store = [&](u16 const offset) -> u32 {
        for (auto const &store: stores) {
            if (store.offset == offset) {
                return store.value;
            }
        }
        return 0xFFFF'FFFF;
    };
    EXPECT_EQ(find_store(virtio::REG_GUEST_FEATURES), 0u);
    EXPECT_EQ(find_store(virtio::REG_QUEUE_SEL), 0u);
    EXPECT_EQ(find_store(virtio::REG_QUEUE_NUM), virtio::LEGACY_QUEUE_NUM);
    EXPECT_EQ(find_store(virtio::REG_QUEUE_PFN),
              std::to_underlying(FakeDevice::REGION_PHYS) / PAGE);
    // GetInfo's answer.
    EXPECT_EQ(device.blocks(), 64u);
}

/// A device that never leaves reset stops the bring-up on TimeOut.
TEST(VirtioDevice, StuckResetTimesOut) {
    auto device_hw = FakeDevice{};
    device_hw.stick_reset();
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    EXPECT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::TimeOut);
}

/// A read: the header and the chain written as the legacy protocol asks,
/// the queue rung, the park answered by the device's completion, and the
/// pattern the device "read" waiting in the data area.
TEST(VirtioDevice, ReadsBlocks) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);

    // The park is where the device acts: it completes the chain and writes
    // the blocks and the status.
    auto const park = [&] {
        device_hw.complete_used();
        device_hw.answer_read(2, 0);
        return Status::Ok;
    };
    ASSERT_EQ(device.read_blocks(2048, 2, park), Status::Ok);

    auto const bytes = std::span<u8 const>{device_hw.region()};
    // The header: a read (type 0) of two blocks from lba 2048.
    u32 const type{0};
    u64 const lba{2048};
    EXPECT_EQ(std::memcmp(bytes.data() + virtio::REQUEST_OFFSET, &type, sizeof(type)), 0);
    EXPECT_EQ(std::memcmp(bytes.data() + virtio::REQUEST_OFFSET + 8, &lba, sizeof(lba)), 0);
    // desc[1]'s WRITE flag is on: the data flows toward the driver.
    u16 const desc1_flags = bytes[1 * 16 + 12] | static_cast<u16>(bytes[1 * 16 + 13] << 8);
    EXPECT_EQ(desc1_flags, static_cast<u16>(virtio::DESC_NEXT | virtio::DESC_WRITE));
    // The data the device wrote, exactly where the reply reads it.
    for (auto i = usize{0}; i < 2 * virtio::SECTOR; ++i) {
        ASSERT_EQ(bytes[virtio::DATA_OFFSET + i], static_cast<u8>(i % 251 + 1)) << i;
    }
    // The queue was rung once, with the queue's index.
    bool rung = false;
    for (auto const &store: device_hw.stores()) {
        if (store.offset == virtio::REG_QUEUE_NOTIFY && store.value == 0) {
            rung = true;
        }
    }
    EXPECT_TRUE(rung);
}

/// A write: the header names a write, the data descriptor's direction is
/// toward the device, and the blocks the driver placed in the data area
/// are what the request carries.
TEST(VirtioDevice, WritesBlocks) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);

    // The driver's data, placed before the call, as the server does.
    auto place = device_hw.region_bytes().subspan(virtio::DATA_OFFSET, virtio::SECTOR);
    for (auto i = usize{0}; i < place.size(); ++i) {
        place[i] = static_cast<u8>(i * 7 + 3);
    }
    // The device reads a write's data, so its answer is the status byte
    // alone: zero blocks of pattern.
    auto const park = [&] {
        device_hw.complete_used();
        device_hw.answer_write(1, 0);
        return Status::Ok;
    };
    ASSERT_EQ(device.write_blocks(16, 1, park), Status::Ok);

    auto const bytes = std::span<u8 const>{device_hw.region()};
    u32 const type{1};
    EXPECT_EQ(std::memcmp(bytes.data() + virtio::REQUEST_OFFSET, &type, sizeof(type)), 0);
    u16 const desc1_flags = bytes[1 * 16 + 12] | static_cast<u16>(bytes[1 * 16 + 13] << 8);
    EXPECT_EQ(desc1_flags, virtio::DESC_NEXT); // the device reads a write's data
    for (auto i = usize{0}; i < virtio::SECTOR; ++i) {
        ASSERT_EQ(bytes[virtio::DATA_OFFSET + i], static_cast<u8>(i * 7 + 3)) << i;
    }
}

/// The status byte a device writes with its completion names the request's
/// failure; a nonzero one reads BadState.
TEST(VirtioDevice, FailedStatusReadsBadState) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);
    auto const park = [&] {
        device_hw.complete_used();
        device_hw.answer_read(1, 1); // the device's own error
        return Status::Ok;
    };
    EXPECT_EQ(device.read_blocks(0, 1, park), Status::BadState);
}

/// No park answers the ring: the submit spins through its park budget and
/// reads TimeOut; a park that itself fails carries its status out.
TEST(VirtioDevice, UnansweredAndFailingParks) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);
    EXPECT_EQ(device.read_blocks(0, 1, [] { return Status::Ok; }), Status::TimeOut);
    EXPECT_EQ(device.read_blocks(0, 1, [] { return Status::BadHandle; }), Status::BadHandle);

    // And the argument guards: no blocks, or more than the region holds.
    EXPECT_EQ(device.read_blocks(0, 0, [] { return Status::Ok; }), Status::InvArgs);
    EXPECT_EQ(device.write_blocks(0, virtio::REGION_BYTES, [] { return Status::Ok; }),
              Status::InvArgs);
}

/// The ring's cursors: a second request's avail index is the counter's
/// next value, free-running -- never reset at the queue's size -- and the
/// completion the test left from the first request does not satisfy the
/// second.
TEST(VirtioDevice, CursorsFreeRun) {
    auto device_hw = FakeDevice{};
    virtio::Device device(device_hw, device_hw.region_map(), FakeDevice::REGION_PHYS);
    ASSERT_EQ(device.bring_up(FakeDevice::REGION_PHYS), Status::Ok);

    auto const park = [&] {
        device_hw.complete_used();
        device_hw.answer_read(1, 0);
        return Status::Ok;
    };
    ASSERT_EQ(device.read_blocks(0, 1, park), Status::Ok);
    ASSERT_EQ(device.read_blocks(1, 1, park), Status::Ok);

    // Two notifications, two completions taken.
    auto notifies = usize{0};
    for (auto const &store: device_hw.stores()) {
        if (store.offset == virtio::REG_QUEUE_NOTIFY) {
            notifies += 1;
        }
    }
    EXPECT_EQ(notifies, 2u);
}

} // namespace
