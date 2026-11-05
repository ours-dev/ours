/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The nvme library on two fakes. The probe runs over a fake ECAM array --
// the walk sees the functions the array lays out, and the sizing hardware's
// answer sheet is the probe-mask table, as in the pci library's own suite.
// The bring-up and the admin commands run against a fake controller: a
// register array whose sink acts on the stores -- CC.EN raises the ready
// bit, the admin doorbell completes the submitted command -- so the stores
// the driver makes have the device's answer, synchronously, as they do on
// silicon. What the cases pin: the probe's match and skips, the sizing and
// assignment of a function that arrived with no base, the CC/CSTS sequence
// and its failures, and each admin command's fields and its failure forms.
#include <nvme/nvme.hpp>

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

/// The fake ECAM: 32 pages, the ones the test names present, plus the
/// sizing hardware's answer sheet -- what each BAR register reads back when
/// all ones are stored to it (the probe write), one word per config word
/// per function, as the pci library's suite models it.
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

/// One admin command the fake saw, the fields its assertions name.
struct Recorded {
    u8 opcode = 0;
    u16 command_id = 0;
    u32 nsid = 0;
    u64 prp1 = 0;
    u32 cdw10 = 0;
    u32 cdw11 = 0;
};

/// The fake controller: the register array and the DMA array, whose sink
/// acts on the stores. CC.EN raises the ready bit (and lowers it again on
/// the disable write); the admin doorbell completes the command the
/// submission rang it for, its answer shaped by the test's modes: |deaf_|
/// completes nothing, |never_ready_| never lets the ready bit rise,
/// |fail_admin_| answers with a status, |fatal_| raises the fatal bit on
/// enable.
class FakeController final: public nvme::Regs::Sink {
  public:
    FakeController() {
        regs_.fill(0);
        dma_.fill(0);
        // CAP's high dword holds DSTRD = 0: the doorbell stride is 4 bytes,
        // QEMU's.
    }

    auto bar0() const -> VirtAddr {
        return VirtAddr{reinterpret_cast<usize>(regs_.data())};
    }
    /// The DMA region's bus address, this model's own fiction: the controller
    /// fetches by it, and the assertions name it through DmaMap's slices.
    auto dma_phys() const -> PhysAddr {
        return PhysAddr{0x2000'0000};
    }
    auto dma_virt() const -> VirtAddr {
        return VirtAddr{reinterpret_cast<usize>(dma_.data())};
    }
    /// The device side, for the Device's register stores to act on.
    auto sink() -> nvme::Regs::Sink * {
        return this;
    }

    /// The registers' words the assertions read back.
    auto reg32(usize const at) const -> u32 {
        u32 value = 0;
        for (auto i = usize{0}; i < 4; ++i) {
            value |= static_cast<u32>(regs_[at + i]) << (8 * i);
        }
        return value;
    }
    auto reg64(usize const at) const -> u64 {
        return static_cast<u64>(reg32(at + 4)) << 32 | reg32(at);
    }

  private:
    auto store32(usize const at, u32 const value) -> void {
        for (auto i = usize{0}; i < 4; ++i) {
            regs_[at + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
    }

  public:
    [[nodiscard]]
    auto cc_writes() const -> std::span<u32 const> {
        return cc_writes_;
    }
    [[nodiscard]]
    auto commands() const -> std::vector<Recorded> const & {
        return commands_;
    }

    auto set_blocks(u64 const blocks) -> void {
        blocks_ = blocks;
    }
    auto make_deaf() -> void {
        deaf_ = true;
    }
    auto never_ready() -> void {
        never_ready_ = true;
    }
    auto never_leaves_ready() -> void {
        never_leaves_ready_ = true;
        // Ready from the start: the disable wait is the one that must time
        // out.
        store32(nvme::NVME_CSTS, static_cast<u32>(nvme::CSTS_READY));
    }
    auto wrong_cid() -> void {
        wrong_cid_ = true;
    }
    auto fail_admin() -> void {
        fail_admin_ = true;
    }
    auto make_fatal() -> void {
        fatal_ = true;
    }

    auto on_write32(usize const at, u32 const value) -> void override {
        if (at == nvme::NVME_CC) {
            cc_writes_.push_back(value);
            // CSTS lives in the register array the driver reads: the model
            // writes the ready bit there, as the device raises it there.
            if ((value & u32{nvme::CC_ENABLE}) != 0) {
                // EN latched: the ready bit rises, and the queue bases are
                // the ones latched from memory -- which is where this model
                // reads them, being the memory.
                auto csts = reg32(nvme::NVME_CSTS);
                if (!never_ready_) {
                    csts |= static_cast<u32>(nvme::CSTS_READY);
                }
                if (fatal_) {
                    csts |= static_cast<u32>(nvme::CSTS_FATAL);
                }
                store32(nvme::NVME_CSTS, csts);
            } else if (!never_leaves_ready_) {
                store32(nvme::NVME_CSTS,
                        reg32(nvme::NVME_CSTS) & ~static_cast<u32>(nvme::CSTS_READY));
            }
            return;
        }
        if (at == nvme::NVME_SQ0TDBL) {
            complete_admin(value);
        }
    }

  private:
    /// Completes the command whose submission rang the doorbell with the
    /// new tail |tail|: the doorbell is the ring's arithmetic, mod the
    /// queue's depth -- a tail of 0 names the slot one before the wrap,
    /// not an empty queue -- so the entry is at (tail + depth - 1) mod
    /// depth, and the completion at the slot the count completed so far
    /// names, its phase bit flipped each full pass -- the driver's expected
    /// phase starts true, so the first pass's completions carry it set.
    auto complete_admin(u32 const tail) -> void {
        if (deaf_) {
            return;
        }
        auto const entry_slot = (tail + nvme::ADMIN_QUEUE_ENTRIES - 1) % nvme::ADMIN_QUEUE_ENTRIES;
        auto const *const entry_bytes =
            dma_.data() + nvme::SLICE_ADMIN_SQ + entry_slot * sizeof(nvme::SqEntry);
        auto entry = nvme::SqEntry{};
        std::memcpy(&entry, entry_bytes, sizeof(entry));
        commands_.push_back(Recorded{.opcode = entry.opcode,
                                     .command_id = entry.command_id,
                                     .nsid = entry.nsid,
                                     .prp1 = entry.prp1,
                                     .cdw10 = entry.cdw10,
                                     .cdw11 = entry.cdw11});
        if (entry.opcode == nvme::ADMIN_IDENTIFY) {
            u64 const nsze = blocks_;
            std::memcpy(dma_.data() + nvme::SLICE_IDENTIFY, &nsze, sizeof(nsze));
        }
        auto const slot = completed_ % nvme::ADMIN_QUEUE_ENTRIES;
        auto const phase =
            (completed_ / nvme::ADMIN_QUEUE_ENTRIES) % 2 == 0 ? u32{0x1'0000} : u32{0};
        auto const status = fail_admin_ ? u32{0x2'0000} : u32{0};
        // A wrong command id is the stale-entry case: the poll reads a
        // fresh completion that answers another command, and spins.
        auto const cid = wrong_cid_ ? static_cast<u16>(entry.command_id + 1) : entry.command_id;
        auto const dw3 = cid | phase | status;
        auto *const cqe = dma_.data() + nvme::SLICE_ADMIN_CQ + slot * sizeof(nvme::CqEntry) + 12;
        std::memcpy(cqe, &dw3, sizeof(dw3));
        ++completed_;
    }

    std::array<u8, nvme::REGS_BYTES> regs_{};
    std::array<u8, nvme::DMA_BYTES> dma_{};
    std::vector<u32> cc_writes_;
    std::vector<Recorded> commands_;
    bool deaf_ = false;
    bool never_ready_ = false;
    bool never_leaves_ready_ = false;
    bool wrong_cid_ = false;
    bool fail_admin_ = false;
    bool fatal_ = false;
    u64 blocks_ = 64;
    usize completed_ = 0;
};

/// A monotonic clock of the tests' own, |step| microseconds per call: the
/// waits measure against it, and a step of a second walks them through
/// their whole timeout in a few dozen iterations.
class FakeClock {
  public:
    explicit FakeClock(u64 const step) : step_(step) {}
    auto operator()() -> u64 {
        auto const now = now_;
        now_ += step_;
        return now;
    }

  private:
    u64 step_ = 0;
    u64 now_ = 0;
};

/// The keepalive the tests hand the waits: how many seconds it was called
/// with, the driver's console line being the target's version of it.
class Ticks {
  public:
    auto operator()(u64 const) -> void {
        ++count_;
    }
    [[nodiscard]]
    auto count() const -> usize {
        return count_;
    }

  private:
    usize count_ = 0;
};

/// The probe: the walk claims the class-01h/08h function with a 64-bit
/// memory BAR0, keeps the firmware's base, and answers NotFound on a bus
/// with none.
TEST(NvmeProbe, ClaimsTheClassMatchAndKeepsItsBase) {
    auto ecam = FakeEcam{};
    ecam.place(3, 0x1AF4, 0x1001, nvme::CLASS_NVM, {0x000F'0004u, 0x0, 0, 0, 0, 0});
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};
    auto config = ecam.config();
    auto const found = nvme::probe(config, arena, {});
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found->index, 3u);
    EXPECT_EQ(std::to_underlying(found->bar0), 0x000F'0000u);
    // Nothing was assigned: the command register is untouched.
    EXPECT_EQ(ecam.word(3, 4), 0xFFFF'FFFFu);
}

TEST(NvmeProbe, NotFoundOnAnEmptyBus) {
    auto ecam = FakeEcam{};
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};
    auto config = ecam.config();
    EXPECT_EQ(nvme::probe(config, arena, {}).unwrap_err(), Status::NotFound);
}

TEST(NvmeProbe, SkipsOwnedAndOtherShapes) {
    auto ecam = FakeEcam{};
    // Two of the class, one with an I/O BAR0 (not a register file this core
    // maps), one with a 32-bit memory BAR0.
    ecam.place(2, 0x1111, 0x1, nvme::CLASS_NVM, {0x1, 0, 0, 0, 0, 0});
    ecam.place(3, 0x1AF4, 0x1001, nvme::CLASS_NVM, {0x000F'0004u, 0x0, 0, 0, 0, 0});
    ecam.place(4, 0x2222, 0x2, nvme::CLASS_NVM, {0x0, 0, 0, 0, 0, 0});
    ecam.place(7, 0x8086, 0x5845, nvme::CLASS_NVM, {0x0010'0004u, 0x0, 0, 0, 0, 0});
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};

    // Owned 3: the walk claims 7, the other shaped one.
    std::array const owned{usize{3}};
    {
        auto config = ecam.config();
        auto const found = nvme::probe(config, arena, std::span<usize const>{owned});
        ASSERT_TRUE(found.is_ok());
        EXPECT_EQ(found->index, 7u);
        EXPECT_EQ(std::to_underlying(found->bar0), 0x0010'0000u);
    }
    // Both owned: nothing to claim.
    std::array const both{usize{3}, usize{7}};
    auto config = ecam.config();
    EXPECT_EQ(nvme::probe(config, arena, std::span<usize const>{both}).unwrap_err(),
              Status::NotFound);
}

/// A function that arrived with no base -- a hot-plugged one -- is sized,
/// assigned from the arena, and enabled: the writes that light it up.
TEST(NvmeProbe, AssignsTheUnassigned) {
    auto ecam = FakeEcam{};
    ecam.place(5, 0x1AF4, 0x1001, nvme::CLASS_NVM, {0x4, 0x0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0xFFFF'C000u); // a 16 KiB decode
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};

    auto config = ecam.config();
    auto const found = nvme::probe(config, arena, {});
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found->index, 5u);
    EXPECT_EQ(std::to_underlying(found->bar0), 0x1000'0000u);
    // The assignment keeps the register's type bits (the 64-bit flag in the
    // low nibble's place) and carries the base.
    EXPECT_EQ(ecam.word(5, 0x10), 0x1000'0004u);
    EXPECT_EQ(ecam.word(5, 0x14), 0x0u); // the base's high half
    // The command register carries io + memory + master: the function
    // decodes and masters.
    EXPECT_EQ(ecam.word(5, 4), 0x7u);
}

/// An arena with no room for the decode is the probe's failure, not a
/// silent overlap.
TEST(NvmeProbe, RefusesWhenTheArenaRunsOut) {
    auto ecam = FakeEcam{};
    ecam.place(5, 0x1AF4, 0x1001, nvme::CLASS_NVM, {0x4, 0x0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0xFFFF'C000u); // a 16 KiB decode
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x1000'2000}};

    auto config = ecam.config();
    EXPECT_EQ(nvme::probe(config, arena, {}).unwrap_err(), Status::NoResource);
}

/// A BAR whose sizing answer names no decode is skipped, not crashed on.
TEST(NvmeProbe, SkipsAnUnprovableBar) {
    auto ecam = FakeEcam{};
    ecam.place(5, 0x1AF4, 0x1001, nvme::CLASS_NVM, {0x4, 0x0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0x4u); // only the type bits: no size
    auto arena = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};
    auto config = ecam.config();
    EXPECT_EQ(nvme::probe(config, arena, {}).unwrap_err(), Status::NotFound);
}

/// The arena's own contract: runs aligned to their size, taken upward, and
/// refused past the limit or of no size at all.
TEST(NvmeArena, AlignsAndRefuses) {
    auto arena = nvme::Arena{PhysAddr{0x1000'0800}, PhysAddr{0x1001'0000}};
    auto const first = arena.take(0x1000);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(std::to_underlying(first.unwrap()), 0x1000'1000u);
    auto const second = arena.take(0x100);
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(std::to_underlying(second.unwrap()), 0x1000'2000u);

    auto cramped = nvme::Arena{PhysAddr{0x1000'F000}, PhysAddr{0x1001'0000}};
    EXPECT_EQ(cramped.take(0x2000).unwrap_err(), Status::NoResource);
    auto any = nvme::Arena{PhysAddr{0x1000'0000}, PhysAddr{0x2000'0000}};
    EXPECT_EQ(any.take(0).unwrap_err(), Status::NoResource);
}

/// The bring-up, the geometry and the I/O queues, against the fake
/// controller: the CC/CSTS sequence in its recorded order, the admin
/// queues' bases from the DMA slices, Identify's NSZE read back, the I/O
/// pair's two commands with their fields, and the block service's views.
TEST(NvmeDevice, BringsUpAndCreatesTheQueues) {
    auto controller = FakeController{};
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;

    ASSERT_EQ(device.bring_up(clock, ticks), Status::Ok);
    // A controller that answers at once feeds the keepalive nothing.
    EXPECT_EQ(ticks.count(), 0u);
    // The CC sequence: disable, the entry sizes, then the sizes with EN.
    ASSERT_EQ(controller.cc_writes().size(), 3u);
    EXPECT_EQ(controller.cc_writes()[0], 0x0u);
    auto const sizes = static_cast<u32>(nvme::CC_IOSQES_VALUE | nvme::CC_IOCQES_VALUE);
    EXPECT_EQ(controller.cc_writes()[1], sizes);
    EXPECT_EQ(controller.cc_writes()[2], sizes | static_cast<u32>(nvme::CC_ENABLE));
    // The admin queues' count and bases, from the DMA slices.
    EXPECT_EQ(controller.reg32(nvme::NVME_AQA), nvme::AQA_ACQS_FIELD | nvme::AQA_ASQS_FIELD);
    EXPECT_EQ(controller.reg64(nvme::NVME_ASQ),
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_ADMIN_SQ);
    EXPECT_EQ(controller.reg64(nvme::NVME_ACQ),
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_ADMIN_CQ);

    // Identify: the NSZE the fake wrote into the Identify page reads back.
    controller.set_blocks(0x1234);
    auto const blocks = device.identify_blocks();
    ASSERT_TRUE(blocks.is_ok());
    EXPECT_EQ(blocks.unwrap(), 0x1234u);
    ASSERT_EQ(controller.commands().size(), 1u);
    EXPECT_EQ(controller.commands()[0].opcode, nvme::ADMIN_IDENTIFY);
    EXPECT_EQ(controller.commands()[0].command_id, 0u);
    EXPECT_EQ(controller.commands()[0].nsid, 1u);
    EXPECT_EQ(controller.commands()[0].cdw10, nvme::CNS_IDENTIFY_NAMESPACE);
    EXPECT_EQ(controller.commands()[0].prp1,
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_IDENTIFY);

    // The I/O pair: the CQ created first, the SQ naming it.
    ASSERT_EQ(device.create_io_queues(), Status::Ok);
    ASSERT_EQ(controller.commands().size(), 3u);
    EXPECT_EQ(controller.commands()[1].opcode, nvme::ADMIN_CREATE_IO_CQ);
    EXPECT_EQ(controller.commands()[1].cdw10,
              static_cast<u32>(nvme::IO_QUEUE_ENTRIES) << 16 | nvme::QID_IO);
    EXPECT_EQ(controller.commands()[1].cdw11, 0x1u);
    EXPECT_EQ(controller.commands()[1].prp1,
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_IO_CQ);
    EXPECT_EQ(controller.commands()[2].opcode, nvme::ADMIN_CREATE_IO_SQ);
    EXPECT_EQ(controller.commands()[2].cdw10,
              static_cast<u32>(nvme::IO_QUEUE_ENTRIES) << 16 | nvme::QID_IO);
    EXPECT_EQ(controller.commands()[2].cdw11, 0x1u | u32{nvme::QID_IO} << 16);
    EXPECT_EQ(controller.commands()[2].prp1,
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_IO_SQ);

    // The block service's views: the data page and the I/O pair's doorbell
    // arithmetic on QEMU's stride.
    EXPECT_EQ(std::to_underlying(device.data()),
              std::to_underlying(controller.dma_virt()) + nvme::SLICE_DATA);
    EXPECT_EQ(std::to_underlying(device.data_phys()),
              std::to_underlying(controller.dma_phys()) + nvme::SLICE_DATA);
    auto &io = device.io();
    (void)io;
}

/// A controller whose ready bit never rises kills the wait on the clock,
/// not on a spin count, and the keepalive sees the seconds tick by.
TEST(NvmeDevice, ReadyWaitTimesOutAndFeedsTheKeepalive) {
    auto controller = FakeController{};
    controller.never_ready();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    // The first wait (the ready bit falling) answers at once; the second
    // runs its whole course on the clock.
    EXPECT_EQ(device.bring_up(clock, ticks), Status::TimeOut);
    EXPECT_GT(ticks.count(), 0u);
}

/// A controller that stays ready through the disable write is one that
/// never left CSTS.RDY: the disable wait is the one that runs out, and
/// bring-up stops before touching the queues.
TEST(NvmeDevice, ReadyThatNeverFallsTimesOut) {
    auto controller = FakeController{};
    // Start ready, and never_leaves_ready keeps the disable write from
    // dropping the bit.
    controller.never_leaves_ready();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    EXPECT_EQ(device.bring_up(clock, ticks), Status::TimeOut);
    // The disable is the only CC write bring-up made.
    ASSERT_EQ(controller.cc_writes().size(), 1u);
    EXPECT_EQ(controller.cc_writes()[0], 0x0u);
}

/// A completion that answers another command's id is a stale entry to the
/// poll: it spins to its round count and reads TimeOut.
TEST(NvmeDevice, WrongCommandIdTimesOutThePoll) {
    auto controller = FakeController{};
    controller.wrong_cid();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    ASSERT_EQ(device.bring_up(clock, ticks), Status::Ok);
    EXPECT_EQ(device.identify_blocks().unwrap_err(), Status::TimeOut);
}

/// A controller that completes nothing holds the command's poll to its
/// spin count, which reads TimeOut.
TEST(NvmeDevice, DeafControllerTimesOutTheCommand) {
    auto controller = FakeController{};
    controller.make_deaf();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    ASSERT_EQ(device.bring_up(clock, ticks), Status::Ok);
    EXPECT_EQ(device.identify_blocks().unwrap_err(), Status::TimeOut);
    EXPECT_EQ(device.create_io_queues(), Status::TimeOut);
}

/// A controller that raises the fatal bit on enable reads BadState.
TEST(NvmeDevice, FatalBitReadsBadState) {
    auto controller = FakeController{};
    controller.make_fatal();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    EXPECT_EQ(device.bring_up(clock, ticks), Status::BadState);
}

/// A controller that answers a command with a status reads BadState, and
/// its answer's phase sequence still runs: the second command of a pass's
/// wrap is answered with the flipped phase bit.
TEST(NvmeDevice, FailedCommandsReadBadState) {
    auto controller = FakeController{};
    controller.fail_admin();
    nvme::Device device(controller.bar0(), controller.dma_phys(), controller.dma_virt(),
                        controller.sink());
    FakeClock clock{1'000'000};
    Ticks ticks;
    ASSERT_EQ(device.bring_up(clock, ticks), Status::Ok);
    EXPECT_EQ(device.identify_blocks().unwrap_err(), Status::BadState);
    EXPECT_EQ(device.create_io_queues(), Status::BadState);
}

} // namespace
