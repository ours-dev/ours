/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The B+tree over blocks: keys and their order, the values' codecs, the
// allocator, lookups and inserts through splits that climb to height 3,
// scans along the leaves' chain, and the refusals each answers.
#include <ours/status.hpp>

#include <array>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <ours_btreefs/ours_btreefs.hpp>

namespace {
using ours::Status;
using ours_btreefs::BLOCK_BYTES;
using ours_btreefs::Item;
using ours_btreefs::Key;
using ours_btreefs::Value;
using ours_btreefs::Volume;

/// A device over memory: the whole volume in blocks, never mapped, exactly
/// what the library's templates ask of a Device.
class FakeDevice final {
  public:
    explicit FakeDevice(u64 const blocks) {
        blocks_[0] = {};
        for (u64 block = 0; block < blocks; ++block) {
            blocks_[block] = {};
        }
    }

    auto read_block(u64 const index, std::span<u8> bytes) -> Status {
        if (bytes.size() != BLOCK_BYTES) {
            return Status::InvArgs;
        }
        auto const at = blocks_.find(index);
        if (at == blocks_.end()) {
            return Status::OutOfRange;
        }
        std::memcpy(bytes.data(), at->second.data(), BLOCK_BYTES);
        return Status::Ok;
    }

    auto write_block(u64 const index, std::span<u8 const> bytes) -> Status {
        if (bytes.size() != BLOCK_BYTES) {
            return Status::InvArgs;
        }
        blocks_[index] = {};
        std::memcpy(blocks_[index].data(), bytes.data(), BLOCK_BYTES);
        return Status::Ok;
    }

  private:
    std::map<u64, std::array<u8, BLOCK_BYTES>> blocks_;
};

/// A volume formatted over a fake device, the pair most tests start from.
struct Mounted {
    FakeDevice device;
    Volume volume;

    explicit Mounted(u64 const blocks, u32 const bitmap_blocks) : device(blocks) {
        // Only EXPECT in a constructor: an ASSERT would return from it, and
        // a failure leaves the default volume for the test to trip over.
        EXPECT_EQ(ours_btreefs::format(device, blocks, bitmap_blocks), Status::Ok);
        auto const opened = ours_btreefs::open(device);
        EXPECT_TRUE(opened.is_ok());
        if (opened.is_ok()) {
            volume = opened.unwrap();
        }
    }
};

/// The key for object |object|'s |type| item at |offset|.
auto key_of(u64 const object, ours_btreefs::ItemType const type, u64 const offset) -> Key {
    return Key{.object = object, .type = type, .offset = offset};
}

/// A value holding one u64 at its start, the cheapest thing to look for.
auto value_of(u64 const marker) -> Value {
    auto value = Value{};
    ours_btreefs::encode_data_value(value, marker);
    return value;
}

/// The u64 a value_of() value holds.
auto marker_of(Value const &value) -> u64 {
    return ours_btreefs::decode_data_value(value);
}

/// Inserts every (object, offset) of |keys| as Data items, expecting Ok.
auto insert_all(Mounted &fs, u64 const object, std::vector<u64> const &offsets) -> void {
    for (auto const offset: offsets) {
        EXPECT_EQ(ours_btreefs::insert(fs.device, fs.volume,
                                       key_of(object, ours_btreefs::ItemType::Data, offset),
                                       value_of(offset * 10)),
                  Status::Ok);
    }
}

// --- Keys and values. ---

TEST(BtreefsKey, OrdersByObjectThenTypeThenOffset) {
    auto const small = key_of(1, ours_btreefs::ItemType::Inode, 0);
    auto const other_object = key_of(2, ours_btreefs::ItemType::Inode, 0);
    auto const other_type = key_of(1, ours_btreefs::ItemType::Dirent, 0);
    auto const other_offset = key_of(1, ours_btreefs::ItemType::Inode, 7);
    EXPECT_LT(small, other_object);
    EXPECT_LT(small, other_type);
    EXPECT_LT(small, other_offset);
    EXPECT_EQ(small, key_of(1, ours_btreefs::ItemType::Inode, 0));
}

TEST(BtreefsKey, TypeAndOffsetShareTheDiskWord) {
    // The type in the top 8 bits and a 56-bit offset below: one past the
    // 56-bit range is refused, the boundary itself survives the round trip.
    Mounted fs(256, 1);
    EXPECT_EQ(ours_btreefs::insert(fs.device, fs.volume,
                                   key_of(1, ours_btreefs::ItemType::Data, u64{1} << 56),
                                   value_of(1)),
              Status::InvArgs);
    auto const edge = u64{1} << 56;
    EXPECT_EQ(ours_btreefs::insert(fs.device, fs.volume,
                                   key_of(1, ours_btreefs::ItemType::Data, edge - 1), value_of(1)),
              Status::Ok);
}

TEST(BtreefsDirent, HashesNamesApart) {
    EXPECT_NE(ours_btreefs::dirent_offset("a"), ours_btreefs::dirent_offset("b"));
    EXPECT_EQ(ours_btreefs::dirent_offset("hello"), ours_btreefs::dirent_offset("hello"));
}

TEST(BtreefsValues, InodeRoundTrips) {
    auto value = Value{};
    ours_btreefs::encode_inode_value(value, 4096, ours_btreefs::ObjectKind::Directory);
    auto const decoded = ours_btreefs::decode_inode_value(value);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->size, u64{4096});
    EXPECT_EQ(decoded->kind, ours_btreefs::ObjectKind::Directory);
}

TEST(BtreefsValues, InodeRefusesAnUnknownKind) {
    auto value = value_of(0);
    value[8] = 9;
    EXPECT_TRUE(ours_btreefs::decode_inode_value(value).is_err());
}

TEST(BtreefsValues, DirentRoundTripsTheName) {
    auto value = Value{};
    ASSERT_EQ(
        ours_btreefs::encode_dirent_value(value, 7, ours_btreefs::ObjectKind::File, "notes.txt"),
        Status::Ok);
    auto const decoded = ours_btreefs::decode_dirent_value(value);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->child, u64{7});
    EXPECT_EQ(decoded->kind, ours_btreefs::ObjectKind::File);
    EXPECT_EQ(decoded->name, "notes.txt");
}

TEST(BtreefsValues, DirentRefusesALongName) {
    auto value = Value{};
    auto const long_name =
        std::string_view("01234567890123456789012345678901234567890123456789012345");
    EXPECT_GT(long_name.size(), ours_btreefs::MAX_NAME_BYTES);
    EXPECT_EQ(
        ours_btreefs::encode_dirent_value(value, 1, ours_btreefs::ObjectKind::File, long_name),
        Status::InvArgs);
}

// --- The allocator. ---

TEST(BtreefsAlloc, HandsOutDistinctBlocksPastTheMetadata) {
    Mounted fs(256, 1);
    auto const first = ours_btreefs::alloc_block(fs.device, fs.volume);
    auto const second = ours_btreefs::alloc_block(fs.device, fs.volume);
    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(first.unwrap(), u64{2});
    EXPECT_EQ(second.unwrap(), u64{3});
}

TEST(BtreefsAlloc, FreesAndHandsTheSameBlockBack) {
    Mounted fs(256, 1);
    auto const block = ours_btreefs::alloc_block(fs.device, fs.volume);
    ASSERT_TRUE(block.is_ok());
    EXPECT_EQ(ours_btreefs::free_block(fs.device, fs.volume, block.unwrap()), Status::Ok);
    auto const again = ours_btreefs::alloc_block(fs.device, fs.volume);
    ASSERT_TRUE(again.is_ok());
    EXPECT_EQ(again.unwrap(), block.unwrap());
}

TEST(BtreefsAlloc, RefusesDoubleFreeAndStrayBlocks) {
    Mounted fs(256, 1);
    EXPECT_EQ(ours_btreefs::free_block(fs.device, fs.volume, 2), Status::BadState);
    EXPECT_EQ(ours_btreefs::free_block(fs.device, fs.volume, 256), Status::InvArgs);
    EXPECT_EQ(ours_btreefs::free_block(fs.device, fs.volume, 0), Status::InvArgs);
}

TEST(BtreefsAlloc, AnswersNoSpaceWhenExhausted) {
    // Three blocks: the superblock and the bitmap are reserved, so exactly
    // one block is there to hand out.
    Mounted fs(3, 1);
    EXPECT_TRUE(ours_btreefs::alloc_block(fs.device, fs.volume).is_ok());
    EXPECT_EQ(ours_btreefs::alloc_block(fs.device, fs.volume).unwrap_err(), Status::NoSpace);
}

// --- Lookup and insert. ---

TEST(BtreefsTree, MissesOnAnEmptyVolume) {
    Mounted fs(256, 1);
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::NotFound);
}

TEST(BtreefsTree, FindsWhatItInserted) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 5, 9});
    auto value = Value{};
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 5),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{50});
}

TEST(BtreefsTree, RefusesADuplicateKey) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1, 2});
    EXPECT_EQ(ours_btreefs::insert(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 1),
                                   value_of(99)),
              Status::AlreadyExists);
    auto value = Value{};
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 1),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{10});
}

TEST(BtreefsTree, SplitsTheLeafAndKeepsEverything) {
    Mounted fs(64, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 2 * ours_btreefs::LEAF_CAPACITY; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);
    EXPECT_EQ(fs.volume.height, 2);
    for (auto const offset: offsets) {
        auto value = Value{};
        ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                       key_of(1, ours_btreefs::ItemType::Data, offset), value),
                  Status::Ok);
        EXPECT_EQ(marker_of(value), offset * 10);
    }
}

TEST(BtreefsTree, SplitsThroughTheRootToHeightThree) {
    Mounted fs(8192, 1);
    auto offsets = std::vector<u64>{};
    // Height 2 holds about 170 leaves' worth, 51 items each: the root
    // splits somewhere past that, and 10'000 ascending inserts reach it.
    for (auto offset = u64{0}; offset < 10'000; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);
    EXPECT_EQ(fs.volume.height, 3);
    for (auto const offset: offsets) {
        auto value = Value{};
        ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                       key_of(1, ours_btreefs::ItemType::Data, offset), value),
                  Status::Ok);
        EXPECT_EQ(marker_of(value), offset * 10);
    }
}

TEST(BtreefsTree, SplitsOnDescendingInsertsToo) {
    Mounted fs(512, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 300; ++offset) {
        offsets.push_back(299 - offset);
    }
    insert_all(fs, 1, offsets);
    for (auto const offset: offsets) {
        auto value = Value{};
        ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                       key_of(1, ours_btreefs::ItemType::Data, offset), value),
                  Status::Ok);
    }
}

TEST(BtreefsTree, KeepsObjectsApart) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1});
    insert_all(fs, 2, {0, 1});
    auto value = Value{};
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(2, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{0});
}

// --- Scans. ---

TEST(BtreefsScan, WalksTheWholeTreeInOrder) {
    Mounted fs(1024, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 300; ++offset) {
        offsets.push_back(offset * 3);
    }
    insert_all(fs, 1, offsets);

    auto seen = std::vector<u64>{};
    auto const from = key_of(1, ours_btreefs::ItemType::Data, 0);
    EXPECT_EQ(ours_btreefs::scan(fs.device, fs.volume, from,
                                 [&](Key const &, Value const &value) -> bool {
                                     seen.push_back(marker_of(value));
                                     return true;
                                 }),
              Status::Ok);
    EXPECT_EQ(seen.size(), offsets.size());
    for (auto index = usize{0}; index < offsets.size(); ++index) {
        EXPECT_EQ(seen[index], offsets[index] * 10);
    }
}

TEST(BtreefsScan, StartsWhereItIsToldAndStopsWhenVisited) {
    Mounted fs(1024, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 300; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);

    auto seen = std::vector<u64>{};
    auto const from = key_of(1, ours_btreefs::ItemType::Data, 250);
    EXPECT_EQ(ours_btreefs::scan(fs.device, fs.volume, from,
                                 [&](Key const &, Value const &value) -> bool {
                                     seen.push_back(marker_of(value));
                                     return seen.size() < usize{3};
                                 }),
              Status::Ok);
    ASSERT_EQ(seen.size(), usize{3});
    EXPECT_EQ(seen[0], u64{2500});
    EXPECT_EQ(seen[1], u64{2510});
    EXPECT_EQ(seen[2], u64{2520});
}

// --- The superblock. ---

TEST(BtreefsVolume, KeepsTheRootAcrossOpen) {
    auto device = FakeDevice(256);
    ASSERT_EQ(ours_btreefs::format(device, 256, 1), Status::Ok);
    auto volume = ours_btreefs::open(device);
    ASSERT_TRUE(volume.is_ok());
    ASSERT_EQ(ours_btreefs::insert(device, volume.unwrap(),
                                   key_of(1, ours_btreefs::ItemType::Inode, 0), value_of(7)),
              Status::Ok);
    auto const reopened = ours_btreefs::open(device);
    ASSERT_TRUE(reopened.is_ok());
    EXPECT_EQ(reopened->root, volume->root);
    EXPECT_EQ(reopened->height, volume->height);

    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(device, reopened.unwrap(),
                                   key_of(1, ours_btreefs::ItemType::Inode, 0), value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{7});
}

TEST(BtreefsVolume, RefusesUnformattedBlocks) {
    auto device = FakeDevice(256);
    EXPECT_EQ(ours_btreefs::open(device).unwrap_err(), Status::NotFound);
}
} // namespace
