/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The PCI configuration space over a fake ECAM array: the walk sees the
// devices the array lays out, and the BAR sizing/assignment arithmetic is
// checked against sizes the test computes by hand -- the arithmetic a
// hot-plugged device's lighting-up depends on. The fake's probe-mask table
// is the sizing hardware's answer sheet: what each BAR register reads back
// when all ones are stored to it (the probe write).
#include <ours/status.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <pci/bar.hpp>
#include <pci/bridge.hpp>
#include <pci/pci.hpp>

namespace {
using ours::Status;
using pci::Config;

/// One ECAM function page: 4 KiB.
constexpr auto PAGE = usize{4096};

/// The fake ECAM: 32 pages, the ones the test names present, plus the
/// sizing hardware's answer sheet -- what each BAR register reads back
/// when all ones are stored to it (the probe write).
class FakeEcam {
  public:
    FakeEcam() {
        // A vacant PCI function reads all ones: the array starts filled.
        bytes_.fill(0xFF);
    }

    /// Lays function |index|'s page in: vendor, device, class code, and the
    /// BAR words the walk reports.
    auto place(usize const index, u16 const vendor, u16 const device, u32 const class_code,
               std::array<u32, 6> const &bars) -> void {
        word(index, 0, static_cast<u32>(vendor) | static_cast<u32>(device) << 16);
        // Config space packs the u32 at offset 8 as class code above the
        // revision byte, so the class goes in shifted up by 8.
        word(index, 8, class_code << 8);
        for (auto i = usize{0}; i < 6; ++i) {
            word(index, 0x10 + i * 4, bars[i]);
        }
    }

    /// The sizing hardware's mask for function |index|'s BAR |bar|: what
    /// the read-back answers the all-ones probe write.
    auto set_mask(usize const index, usize const bar, u32 const mask) -> void {
        masks_[(0x10 + bar * 4) / 4 + index * (PAGE / 4)] = mask;
    }

    auto config() -> Config {
        return Config{std::span{bytes_}, masks_};
    }

    [[nodiscard]]
    auto raw() const -> std::span<u8 const> {
        return std::span{bytes_};
    }

    /// The raw word of function |index|'s register |offset|: what the
    /// assignment left behind.
    auto word(usize const index, usize const offset) const -> u32 {
        usize value = 0;
        for (auto i = usize{0}; i < 4; ++i) {
            value |= static_cast<u32>(bytes_[index * PAGE + offset + i]) << (8 * i);
        }
        return static_cast<u32>(value);
    }

    /// Stores a raw word into function |index|'s register |offset|, for
    /// the registers a test seeds that place() does not carry.
    auto word_at(usize const index, usize const offset, u32 const value) -> void {
        for (auto i = usize{0}; i < 4; ++i) {
            bytes_[index * PAGE + offset + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
        }
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

/// The id of the class directory's child named |name|, over the public
/// walk: the test's stand-in for the tree's private lookup.
TEST(IoservicePciConfig, ReadsBytesAndSpansBuses) {
    auto ecam = FakeEcam{};
    ecam.place(9, 0x1B36, 0x000C, 0x060400, {0, 0, 0, 0, 0, 0});
    auto config = ecam.config();
    // A byte register: the interrupt pin the drivers' swizzle reads.
    EXPECT_EQ(config.read8(9, 0x3C), 0xFFu); // vacant page bytes read all ones
    // The bus slice a multi-bus driver makes its per-bus Configs over: one
    // Config spans BUS_BYTES, and the next bus's window starts past it.
    EXPECT_EQ(pci::BUS_BYTES, pci::FUNCTIONS_PER_BUS * pci::FUNCTION_STRIDE);
}

TEST(IoservicePciWalk, VisitsPresentFunctionsInOrder) {
    auto ecam = FakeEcam{};
    ecam.place(3, 0x1AF4, 0x1001, 0x010000, {0x1, 0, 0, 0, 0, 0});
    ecam.place(7, 0x8086, 0x5845, 0x010802, {0x0, 0, 0, 0, 0, 0});

    auto visited = std::vector<usize>{};
    pci::walk_bus0(ecam.config(), [&](pci::Function const &fn) {
        visited.push_back(fn.index);
        if (fn.index == 3) {
            EXPECT_EQ(fn.vendor, 0x1AF4);
            EXPECT_EQ(fn.device, 0x1001);
            EXPECT_EQ(fn.class_code, 0x010000u);
            EXPECT_EQ(fn.bars[0], 0x1u); // an I/O BAR
        } else {
            EXPECT_EQ(fn.vendor, 0x8086);
            EXPECT_EQ(fn.class_code, 0x010802u); // NVMe
        }
        return true;
    });
    EXPECT_EQ(visited, (std::vector<usize>{3, 7}));
}

/// The visit callback's false stops the walk early.
TEST(IoservicePciWalk, VisitFalseStopsEarly) {
    auto ecam = FakeEcam{};
    ecam.place(1, 0x1111, 0x1, 0, {0, 0, 0, 0, 0, 0});
    ecam.place(2, 0x2222, 0x2, 0, {0, 0, 0, 0, 0, 0});
    ecam.place(3, 0x3333, 0x3, 0, {0, 0, 0, 0, 0, 0});

    auto visited = std::vector<usize>{};
    pci::walk_bus0(ecam.config(), [&](pci::Function const &fn) {
        visited.push_back(fn.index);
        return fn.index != 2; // false at 2: the walk must never reach 3
    });
    EXPECT_EQ(visited, (std::vector<usize>{1, 2}));
}

/// A memory BAR's sizing: the answer sheet says 16 KiB (the low 15 bits of
/// the mask clear), and the size comes out 0x8000.
TEST(IoservicePciBar, MemoryBarSize) {
    auto ecam = FakeEcam{};
    ecam.place(5, 0x1234, 0x5678, 0x010000, {0x1000'0000u /* 32-bit memory */, 0, 0, 0, 0, 0});
    ecam.set_mask(5, 0, 0xFFFF'8000u);
    auto config = ecam.config();
    (void)config;

    auto const sized = pci::size_bar(config, 5, 0);
    ASSERT_TRUE(sized.is_ok());
    EXPECT_EQ(sized->space, pci::Space::Memory);
    EXPECT_EQ(sized->size, usize{0x8000});
    EXPECT_EQ(sized->is64, false);
}

/// An I/O BAR's sizing, same table: a 16-port decode.
TEST(IoservicePciBar, IoBarSize) {
    auto ecam = FakeEcam{};
    ecam.place(2, 0x1AF4, 0x1001, 0x010000, {0x1, 0, 0, 0, 0, 0});
    ecam.set_mask(2, 0, 0xFFFF'FFF1u);
    auto config = ecam.config();
    (void)config;

    auto const sized = pci::size_bar(config, 2, 0);
    ASSERT_TRUE(sized.is_ok());
    EXPECT_EQ(sized->space, pci::Space::Io);
    EXPECT_EQ(sized->size, usize{16});
}

/// A 64-bit memory BAR: the probe reads the high half too, and the size
/// comes from the two halves' mask as one 64-bit answer. The high mask of
/// a real 16 KiB decode is all ones -- every bit above the low half's is a
/// size bit -- which the per-half arithmetic reads as four gibibytes of
/// high contribution; the whole-mask complement reads it as the 16 KiB it
/// is.
TEST(IoservicePciBar, HighHalfJoinsTheSize) {
    auto ecam = FakeEcam{};
    // The high half lies at 0 (a base below 4 GiB so far) -- distinct from
    // its probe mask, so the size must come from the probe, not the
    // restored word.
    ecam.place(6, 0x1234, 0x5678, 0x010000, {0x4000'0004u, 0x0, 0, 0, 0, 0});
    ecam.set_mask(6, 0, 0xFFFF'C000u);
    ecam.set_mask(6, 1, 0xFFFF'FFFFu);
    auto config = ecam.config();
    (void)config;

    auto const sized = pci::size_bar(config, 6, 0);
    ASSERT_TRUE(sized.is_ok());
    EXPECT_EQ(sized->is64, true);
    EXPECT_EQ(sized->size, usize{0x4000});
}

/// A decode that crosses 4 GiB: the low mask's own complement overflows
/// the low half, and the high half's mask carries the rest -- 8 GiB.
TEST(IoservicePciBar, SizeCrossesTheFourGibByteLine) {
    auto ecam = FakeEcam{};
    ecam.place(6, 0x1234, 0x5678, 0x010000, {0x4u, 0x0, 0, 0, 0, 0});
    ecam.set_mask(6, 0, 0x0u);
    ecam.set_mask(6, 1, 0xFFFF'FFFEu);
    auto config = ecam.config();

    auto const sized = pci::size_bar(config, 6, 0);
    ASSERT_TRUE(sized.is_ok());
    EXPECT_EQ(sized->is64, true);
    EXPECT_EQ(sized->size, usize{2} << 32);
}

/// Assignment: assign_bar writes the base keeping the register's fixed low
/// bits, and enable_function sets the command word's decode and master
/// bits -- the two writes that light a hot-plugged function up.
TEST(IoservicePciBar, AssignAndEnable) {
    auto ecam = FakeEcam{};
    ecam.place(4, 0x1234, 0x5678, 0x010000, {0x0 /* 32-bit memory BAR */, 0, 0, 0, 0, 0});
    pci::Bar const shape{
        .space = pci::Space::Memory, .size = usize{0x1000}, .index = 0, .is64 = false};
    u64 const BASE = 0xF00'0000;

    auto config = ecam.config();
    pci::assign_bar(config, 4, 0, shape, BASE);
    // The low bits keep the register's decode type (memory: bits 1:0 are 0).
    EXPECT_EQ(ecam.word(4, 0x10), BASE);
    pci::enable_function(config, 4);
    EXPECT_EQ((ecam.word(4, 4) & 0x7u), 0x7u); // io + memory + master

    // A 64-bit BAR: the high half carries the upper base bits.
    u64 const HIGH_BASE = 0x40'0000'0000ull;
    pci::Bar const high{
        .space = pci::Space::Memory, .size = usize{0x1000}, .index = 0, .is64 = true};
    pci::assign_bar(config, 4, 0, high, HIGH_BASE);
    EXPECT_EQ(ecam.word(4, 0x10), static_cast<u32>(HIGH_BASE & 0xFFFF'FFFF));
    EXPECT_EQ(ecam.word(4, 0x14), static_cast<u32>(HIGH_BASE >> 32));
}

/// The guards around sizing: the register numbers are bounds-checked before
/// any window touch, and a BAR whose probe answer carries only flag bits --
/// no size bits -- names no decode and is refused.
TEST(IoservicePciBar, RefusesBoundsAndBlankProbeAnswers) {
    auto ecam = FakeEcam{};
    auto config = ecam.config();
    EXPECT_TRUE(pci::size_bar(config, 256, 0).is_err()); // no function page 256
    EXPECT_TRUE(pci::size_bar(config, 0, 6).is_err());  // no BAR register 6

    // An I/O BAR that answers all ones with just the space flag: no size.
    ecam.place(1, 0x1111, 0x1, 0, {0x1, 0, 0, 0, 0, 0});
    ecam.set_mask(1, 0, 0x1);
    EXPECT_TRUE(pci::size_bar(config, 1, 0).is_err());

    // A memory BAR whose probe answer holds only the type bits: no size.
    ecam.place(2, 0x2222, 0x2, 0, {0x0, 0, 0, 0, 0, 0});
    ecam.set_mask(2, 0, 0x4);
    EXPECT_TRUE(pci::size_bar(config, 2, 0).is_err());
}

/// The bridge windows: what a hot-added function needs besides its own
/// BARs. The bridge to a bus is found by its secondary-bus register, the
/// memory and I/O windows are written in their fields' granularity, the
/// subordinate bus is named, and the bridge's command is armed.
TEST(IoservicePciBridge, ProgramsTheWindows) {
    auto ecam = FakeEcam{};
    // The root port: a bridge on bus 0 at device 4, secondary bus 1.
    ecam.place(4 * 8, 0x1B36, 0x000C, 0x060400, {0, 0, 0, 0, 0, 0});
    ecam.word_at(4 * 8, 0x18, 0x0000'0100u); // primary 0, secondary 1
    auto bus0 = ecam.config();

    auto const bridge = pci::bridge_for_bus(bus0, 1);
    ASSERT_TRUE(bridge.is_ok());
    EXPECT_EQ(bridge.unwrap(), 4u * 8u);

    pci::program_bridge_memory(bus0, bridge.unwrap(), 0xAC00'0000u, 0xB000'0000u, 1);
    // The window fields: the base's and limit's address bits 31:20, each
    // in its register's own bit position -- 0xAC00 and 0xAFF0.
    EXPECT_EQ(ecam.word(4 * 8, 0x20), 0xAFF0'AC00u);
    // The bus numbers dword: the subordinate named, the rest kept.
    EXPECT_EQ(ecam.word(4 * 8, 0x18), 0x0001'0100u);

    pci::program_bridge_io(bus0, bridge.unwrap(), 0xE000, 0xF000);
    // The I/O fields: address bits 15:12, the low nibbles the 32-bit flag.
    EXPECT_EQ(ecam.word(4 * 8, 0x1C), 0x0000'E1E1u);

    pci::enable_bridge(bus0, bridge.unwrap());
    EXPECT_EQ((ecam.word(4 * 8, 4) & 0x7u), 0x7u);

    // No bridge to an unprovisioned bus.
    EXPECT_TRUE(pci::bridge_for_bus(bus0, 2).is_err());
}

} // namespace
