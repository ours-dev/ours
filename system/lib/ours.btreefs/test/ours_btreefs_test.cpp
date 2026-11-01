/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The copy-on-write B+tree: keys and their order, the values' codecs, the
// allocator's bits, lookups and inserts through splits that climb to
// height 3, removals through borrows and merges, scans along the leaves'
// chain -- every mutation inside a transaction, with the tests for what a
// transaction owes: the committed tree untouched until commit(), the crash
// rehearsal costing nothing, and the two superblock slots taking turns.
#include <ours/status.hpp>

#include <array>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <cstring>

#include <gtest/gtest.h>
#include <ours_btreefs/ours_btreefs.hpp>

namespace {
using ours::Status;
using ours_btreefs::BLOCK_BYTES;
using ours_btreefs::Key;
using ours_btreefs::Value;
using ours_btreefs::Volume;

/// A device over memory: the whole volume in blocks, never mapped, exactly
/// what the library's templates ask of a Device.
class FakeDevice final {
  public:
    explicit FakeDevice(u64 const blocks) {
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

/// Runs |body| as one transaction over |fs|, committing what it did. The
/// shape every mutation test takes, and the shape the component's dispatch
/// takes on the booted system.
template <typename Body>
auto apply(Mounted &fs, Body &&body) -> Status {
    auto tx = ours_btreefs::Transaction(fs.device, fs.volume);
    auto const status = body(tx);
    if (status != Status::Ok) {
        return status;
    }
    return tx.commit();
}

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

/// Inserts every (object, offset) of |keys| as Data items, one transaction
/// each, expecting Ok.
auto insert_all(Mounted &fs, u64 const object, std::vector<u64> const &offsets) -> void {
    for (auto const offset: offsets) {
        EXPECT_EQ(apply(fs,
                        [&](auto &tx) {
                            return ours_btreefs::insert(
                                tx, key_of(object, ours_btreefs::ItemType::Data, offset),
                                value_of(offset * 10));
                        }),
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
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::insert(
                            tx, key_of(1, ours_btreefs::ItemType::Data, u64{1} << 56), value_of(1));
                    }),
              Status::InvArgs);
    auto const edge = u64{1} << 56;
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::insert(
                            tx, key_of(1, ours_btreefs::ItemType::Data, edge - 1), value_of(1));
                    }),
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

// --- The allocator, whose bits a transaction's commit writes. ---

TEST(BtreefsAlloc, HandsOutDistinctBlocksPastTheMetadata) {
    Mounted fs(256, 1);
    auto first = u64{0};
    auto second = u64{0};
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const one = tx.alloc_block();
                        auto const two = tx.alloc_block();
                        first = one.unwrap();
                        second = two.unwrap();
                        return Status::Ok;
                    }),
              Status::Ok);
    EXPECT_EQ(first, u64{3});
    EXPECT_EQ(second, u64{4});
}

TEST(BtreefsAlloc, FreesAndHandsTheSameBlockBack) {
    Mounted fs(256, 1);
    auto block = u64{0};
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const taken = tx.alloc_block();
                        block = taken.unwrap();
                        return Status::Ok;
                    }),
              Status::Ok);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(block); }), Status::Ok);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const again = tx.alloc_block();
                        EXPECT_EQ(again.unwrap(), block);
                        return Status::Ok;
                    }),
              Status::Ok);
}

TEST(BtreefsAlloc, RefusesDoubleFreeAndStrayBlocks) {
    Mounted fs(256, 1);
    auto block = u64{0};
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        block = tx.alloc_block().unwrap();
                        return Status::Ok;
                    }),
              Status::Ok);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(block); }), Status::Ok);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(block); }), Status::BadState);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(256); }), Status::InvArgs);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(0); }), Status::InvArgs);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return tx.free_block(1); }), Status::InvArgs);
}

TEST(BtreefsAlloc, AnswersNoSpaceWhenExhausted) {
    // Four blocks: the two superblock slots and the bitmap are reserved, so
    // exactly one block is there to hand out.
    Mounted fs(4, 1);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const first = tx.alloc_block();
                        EXPECT_TRUE(first.is_ok());
                        auto const second = tx.alloc_block();
                        EXPECT_EQ(second.unwrap_err(), Status::NoSpace);
                        return Status::Ok;
                    }),
              Status::Ok);
}

TEST(BtreefsAlloc, AnAbandonedTakeIsFreeAgain) {
    // The transaction's own rule: bits are written at commit, so blocks a
    // dropped transaction took never left the free set.
    Mounted fs(256, 1);
    auto const taken = [&] {
        auto tx = ours_btreefs::Transaction(fs.device, fs.volume);
        auto const block = tx.alloc_block();
        EXPECT_TRUE(block.is_ok());
        return block.unwrap();
    }();
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const again = tx.alloc_block();
                        EXPECT_EQ(again.unwrap(), taken);
                        return Status::Ok;
                    }),
              Status::Ok);
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
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::insert(tx, key_of(1, ours_btreefs::ItemType::Data, 1),
                                                    value_of(99));
                    }),
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

TEST(BtreefsTree, UpdatesAValueInPlace) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1, 2});
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::update(tx, key_of(1, ours_btreefs::ItemType::Data, 1),
                                                    value_of(99));
                    }),
              Status::Ok);
    auto value = Value{};
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 1),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{99});
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{0});
}

TEST(BtreefsTree, UpdateMissesWhatIsNotThere) {
    Mounted fs(256, 1);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::update(tx, key_of(1, ours_btreefs::ItemType::Data, 0),
                                                    value_of(1));
                    }),
              Status::NotFound);
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

// --- Removal. ---

TEST(BtreefsRemove, MissesWhatIsNotThere) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1});
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::remove(tx, key_of(1, ours_btreefs::ItemType::Data, 5));
                    }),
              Status::NotFound);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::remove(tx, key_of(9, ours_btreefs::ItemType::Data, 0));
                    }),
              Status::NotFound);
}

TEST(BtreefsRemove, TakesTheOneItNames) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1, 2});
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::remove(tx, key_of(1, ours_btreefs::ItemType::Data, 1));
                    }),
              Status::Ok);
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 1),
                                   value),
              Status::NotFound);
    ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{0});
}

TEST(BtreefsRemove, BorrowKeepsTheParentTrue) {
    // Three leaves: fill one past two minimums, then remove from the middle
    // until it must borrow, left and right both. Every lookup after each
    // step is the check that the parent's separators still route.
    Mounted fs(512, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 3 * ours_btreefs::LEAF_MIN + 6; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);
    ASSERT_EQ(fs.volume.height, 2);
    // Remove the middle leaf's items down past its minimum: the middle is
    // neither the first nor the last leaf, so both borrows get exercised.
    auto const mid_start = ours_btreefs::LEAF_MIN + 1;
    for (auto offset = mid_start; offset < mid_start + ours_btreefs::LEAF_MIN; ++offset) {
        EXPECT_EQ(apply(fs,
                        [&](auto &tx) {
                            return ours_btreefs::remove(
                                tx, key_of(1, ours_btreefs::ItemType::Data, offset));
                        }),
                  Status::Ok);
        auto value = Value{};
        for (auto const kept: offsets) {
            if (mid_start <= kept && kept <= offset) {
                continue;
            }
            ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                           key_of(1, ours_btreefs::ItemType::Data, kept), value),
                      Status::Ok);
        }
    }
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                   key_of(1, ours_btreefs::ItemType::Data, mid_start), value),
              Status::NotFound);
}

TEST(BtreefsRemove, MergeCascadeShrinksTheTree) {
    // Height 3 by mass, then every item out: the tree must come back to a
    // single leaf, and the allocator must have everything but that leaf.
    Mounted fs(8192, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 10'000; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);
    ASSERT_EQ(fs.volume.height, 3);

    for (auto const offset: offsets) {
        auto const removed = apply(fs, [&](auto &tx) {
            return ours_btreefs::remove(tx, key_of(1, ours_btreefs::ItemType::Data, offset));
        });
        ASSERT_EQ(removed, Status::Ok);
    }
    EXPECT_EQ(fs.volume.height, 1);
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::NotFound);

    // Every node's block went back but the root leaf's, which the tree
    // still lives in: the volume hands out the rest, one block per
    // transaction (a transaction's taken list is finite).
    auto free_seen = usize{0};
    while (true) {
        auto const taken = apply(fs, [&](auto &tx) {
            auto const block = tx.alloc_block();
            if (block.is_err()) {
                return block.unwrap_err();
            }
            return Status::Ok;
        });
        if (taken != Status::Ok) {
            EXPECT_EQ(taken, Status::NoSpace);
            break;
        }
        free_seen += 1;
    }
    EXPECT_EQ(free_seen, usize{8192 - 4});
}

TEST(BtreefsRemove, ScanStaysOrderedPastRemovals) {
    Mounted fs(1024, 1);
    auto offsets = std::vector<u64>{};
    for (auto offset = u64{0}; offset < 500; ++offset) {
        offsets.push_back(offset);
    }
    insert_all(fs, 1, offsets);
    for (auto offset = u64{0}; offset < 500; offset += 2) {
        ASSERT_EQ(apply(fs,
                        [&](auto &tx) {
                            return ours_btreefs::remove(
                                tx, key_of(1, ours_btreefs::ItemType::Data, offset));
                        }),
                  Status::Ok);
    }
    auto seen = std::vector<u64>{};
    EXPECT_EQ(ours_btreefs::scan(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                 [&](Key const &, Value const &value) -> bool {
                                     seen.push_back(marker_of(value));
                                     return true;
                                 }),
              Status::Ok);
    ASSERT_EQ(seen.size(), offsets.size() / 2);
    for (auto index = usize{0}; index < seen.size(); ++index) {
        EXPECT_EQ(seen[index], u64{index} * 20 + 10);
    }
}

TEST(BtreefsRemove, KeepsTheRootLeafBlock) {
    // Removing the last item of a root leaf leaves the tree one empty leaf:
    // its block is still the tree's, and the allocator hands out the next.
    Mounted fs(8, 1);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::insert(tx, key_of(1, ours_btreefs::ItemType::Inode, 0),
                                                    value_of(1));
                    }),
              Status::Ok);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::remove(tx,
                                                    key_of(1, ours_btreefs::ItemType::Inode, 0));
                    }),
              Status::Ok);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        auto const block = tx.alloc_block();
                        EXPECT_EQ(block.unwrap(), u64{4});
                        return Status::Ok;
                    }),
              Status::Ok);
}

TEST(BtreefsTruncate, TakesTheBlocksFromTheCutOnward) {
    Mounted fs(256, 1);
    // The DATA items' values are physical blocks, the allocator's: truncate
    // hands them back, so they are real here.
    auto physical = std::vector<u64>{};
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        for (auto offset = u64{0}; offset < 6; ++offset) {
                            auto const block = tx.alloc_block();
                            EXPECT_TRUE(block.is_ok());
                            physical.push_back(block.unwrap());
                            EXPECT_EQ(ours_btreefs::insert(
                                          tx, key_of(1, ours_btreefs::ItemType::Data, offset),
                                          value_of(block.unwrap())),
                                      Status::Ok);
                        }
                        return Status::Ok;
                    }),
              Status::Ok);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return ours_btreefs::truncate(tx, 1, 2); }), Status::Ok);
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 0),
                                   value),
              Status::Ok);
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 1),
                                   value),
              Status::Ok);
    for (auto const offset: {u64{2}, u64{3}, u64{4}, u64{5}}) {
        EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                       key_of(1, ours_btreefs::ItemType::Data, offset), value),
                  Status::NotFound);
    }
    // The four blocks of the cut items are back: the next four allocations
    // are distinct and none is a kept item's block -- the freed ones are
    // the only four to hand out.
    auto handed = std::vector<u64>{};
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        for (usize left = 4; left > 0; --left) {
                            auto const block = tx.alloc_block();
                            EXPECT_TRUE(block.is_ok());
                            handed.push_back(block.unwrap());
                        }
                        auto const one_more = tx.alloc_block();
                        EXPECT_TRUE(one_more.is_ok());
                        handed.push_back(one_more.unwrap());
                        return Status::Ok;
                    }),
              Status::Ok);
    EXPECT_EQ(handed.size(), usize{5});
    for (auto const block: handed) {
        EXPECT_NE(block, physical[0]);
        EXPECT_NE(block, physical[1]);
    }
    for (auto index = usize{0}; index < handed.size(); ++index) {
        for (auto other = usize{0}; other < index; ++other) {
            EXPECT_NE(handed[index], handed[other]);
        }
    }
}

TEST(BtreefsTruncate, OfWhatIsNotThereIsQuiet) {
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1});
    EXPECT_EQ(apply(fs, [&](auto &tx) { return ours_btreefs::truncate(tx, 1, 9); }), Status::Ok);
    EXPECT_EQ(apply(fs, [&](auto &tx) { return ours_btreefs::truncate(tx, 9, 0); }), Status::Ok);
}

// --- The transaction's own promises. ---

TEST(BtreefsTransaction, AnAbandonedInsertChangedNothing) {
    // The rehearsal rule: insert, never commit, and the tree, its size and
    // the superblock's generation are what they were.
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1, 2});
    auto const generation = fs.volume.generation;
    auto const root = fs.volume.root;
    {
        auto tx = ours_btreefs::Transaction(fs.device, fs.volume);
        EXPECT_EQ(
            ours_btreefs::insert(tx, key_of(1, ours_btreefs::ItemType::Data, 3), value_of(30)),
            Status::Ok);
        // dropped, not committed
    }
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, fs.volume, key_of(1, ours_btreefs::ItemType::Data, 3),
                                   value),
              Status::NotFound);
    EXPECT_EQ(fs.volume.generation, generation);
    EXPECT_EQ(fs.volume.root, root);
    for (auto const kept: {u64{0}, u64{1}, u64{2}}) {
        ASSERT_EQ(ours_btreefs::lookup(fs.device, fs.volume,
                                       key_of(1, ours_btreefs::ItemType::Data, kept), value),
                  Status::Ok);
    }
}

TEST(BtreefsTransaction, CommitCountsTheGenerationsAndSwapsTheSlots) {
    // A generation-N commit writes slot N % 2: the two commits here land on
    // different slots, and the slot one commit behind still reads the state
    // it carried then.
    Mounted fs(256, 1);
    insert_all(fs, 1, {0});
    auto const after_one = fs.volume.generation;
    auto slot = std::array<u8, BLOCK_BYTES>{};
    EXPECT_EQ(fs.device.read_block(0, slot), Status::Ok);
    auto const at_slot_zero = ours_btreefs::detail::load_superblock(slot);
    ASSERT_TRUE(at_slot_zero.is_ok());
    EXPECT_EQ(at_slot_zero->generation, after_one);
    insert_all(fs, 1, {1});
    EXPECT_EQ(fs.volume.generation, after_one + 1);
    // The second commit took the other slot; this one still reads the first
    // commit's state, one generation behind.
    EXPECT_EQ(fs.device.read_block(0, slot), Status::Ok);
    auto const stale = ours_btreefs::detail::load_superblock(slot);
    ASSERT_TRUE(stale.is_ok());
    EXPECT_EQ(stale->generation, after_one);
}

TEST(BtreefsTransaction, OpensTheSurvivorOfATornSlot) {
    // The crash's last stand: one superblock slot torn to garbage, the
    // other carrying the live state. open() answers the survivor.
    Mounted fs(256, 1);
    insert_all(fs, 1, {0, 1, 2});
    // The live slot carries generation 2, so the stale one is block 1.
    auto trash = std::array<u8, BLOCK_BYTES>{};
    std::memset(trash.data(), 0xEE, trash.size());
    EXPECT_EQ(fs.device.write_block(1, trash), Status::Ok);
    auto const reopened = ours_btreefs::open(fs.device);
    ASSERT_TRUE(reopened.is_ok());
    EXPECT_EQ(reopened->root, fs.volume.root);
    auto value = Value{};
    ASSERT_EQ(ours_btreefs::lookup(fs.device, reopened.unwrap(),
                                   key_of(1, ours_btreefs::ItemType::Data, 2), value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{20});
}

// --- The superblock. ---

TEST(BtreefsVolume, HandsOutObjectIdsInOrderAndKeepsThem) {
    Mounted fs(256, 1);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        EXPECT_EQ(tx.alloc_object(), u64{1});
                        EXPECT_EQ(tx.alloc_object(), u64{2});
                        return Status::Ok;
                    }),
              Status::Ok);
    auto const reopened = ours_btreefs::open(fs.device);
    ASSERT_TRUE(reopened.is_ok());
    EXPECT_EQ(reopened->next_object, u64{3});
}

TEST(BtreefsVolume, KeepsTheRootAcrossOpen) {
    Mounted fs(256, 1);
    EXPECT_EQ(apply(fs,
                    [&](auto &tx) {
                        return ours_btreefs::insert(tx, key_of(1, ours_btreefs::ItemType::Inode, 0),
                                                    value_of(7));
                    }),
              Status::Ok);
    auto const reopened = ours_btreefs::open(fs.device);
    ASSERT_TRUE(reopened.is_ok());
    EXPECT_EQ(reopened->root, fs.volume.root);
    EXPECT_EQ(reopened->height, fs.volume.height);
    auto value = Value{};
    EXPECT_EQ(ours_btreefs::lookup(fs.device, reopened.unwrap(),
                                   key_of(1, ours_btreefs::ItemType::Inode, 0), value),
              Status::Ok);
    EXPECT_EQ(marker_of(value), u64{7});
}

TEST(BtreefsVolume, RefusesUnformattedBlocks) {
    auto device = FakeDevice(256);
    EXPECT_EQ(ours_btreefs::open(device).unwrap_err(), Status::NotFound);
}

TEST(BtreefsVolume, RefusesASlotWhoseCrcLies) {
    // The magic present but the bytes not what the CRC vouched for: the
    // slot is rejected, and open() answers the other one.
    Mounted fs(256, 1);
    insert_all(fs, 1, {0});
    // The live slot carries generation 2; the stale one is block 1, whose
    // bytes are tampered with.
    auto slot = std::array<u8, BLOCK_BYTES>{};
    EXPECT_EQ(fs.device.read_block(1, slot), Status::Ok);
    slot[100] = static_cast<u8>(slot[100] ^ 0xFF);
    EXPECT_EQ(fs.device.write_block(1, slot), Status::Ok);
    auto const reopened = ours_btreefs::open(fs.device);
    ASSERT_TRUE(reopened.is_ok());
    EXPECT_EQ(reopened->root, fs.volume.root);
}
} // namespace
