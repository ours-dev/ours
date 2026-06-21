/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ours/lib/xarray.hpp>

#include <gtest/gtest.h>

namespace {
    using namespace ours;

    using ours::Status;
    using ours::lib::XArray;

    struct Object {
        usize value {};
    };

    TEST(XArray, StoreAndLoadSparseObjects) {
        XArray xa;
        Object a {11};
        Object b {22};
        Object c {33};

        ASSERT_TRUE(xa.empty());
        ASSERT_EQ(xa.store(0, &a), Status::Ok);
        ASSERT_EQ(xa.store(4096, &b), Status::Ok);
        ASSERT_EQ(xa.store(1_usize << 20, &c), Status::Ok);

        EXPECT_FALSE(xa.empty());
        EXPECT_EQ(xa.load<Object>(0), &a);
        EXPECT_EQ(xa.load<Object>(4096), &b);
        EXPECT_EQ(xa.load<Object>(1_usize << 20), &c);
        EXPECT_EQ(xa.load(123), nullptr);
    }

    TEST(XArray, StoreModes) {
        XArray xa;
        Object a {1};
        Object b {2};
        Object c {3};

        EXPECT_EQ(xa.store(7, &a, XArray::StoreMode::Update), Status::NotFound);
        EXPECT_EQ(xa.store(7, &a, XArray::StoreMode::Create), Status::Ok);
        EXPECT_EQ(xa.store(7, &b, XArray::StoreMode::Create), Status::AlreadyExists);
        EXPECT_EQ(xa.store(7, &c, XArray::StoreMode::Update), Status::Ok);
        EXPECT_EQ(xa.load<Object>(7), &c);

        XArray sparse;
        EXPECT_EQ(sparse.store(4096, &a, XArray::StoreMode::Update), Status::NotFound);
        EXPECT_TRUE(sparse.empty());
    }

    TEST(XArray, ErasePrunesEntries) {
        XArray xa;
        Object a {10};
        Object b {20};

        ASSERT_EQ(xa.store(0, &a), Status::Ok);
        ASSERT_EQ(xa.store(128, &b), Status::Ok);

        auto old = xa.erase(0);
        EXPECT_EQ(old, &a);
        EXPECT_EQ(xa.load(0), nullptr);
        EXPECT_FALSE(xa.empty());

        old = xa.erase(128);
        EXPECT_EQ(old, &b);
        EXPECT_TRUE(xa.empty());
    }

    TEST(XArray, MarksAndFindMarked) {
        XArray xa;
        Object a {30};
        Object b {64};
        Object c {97};

        ASSERT_EQ(xa.store(3, &a), Status::Ok);
        ASSERT_EQ(xa.store(64, &b), Status::Ok);
        ASSERT_EQ(xa.store(4097, &c), Status::Ok);

        EXPECT_EQ(xa.set_mark(64, XArray::Mark::Mark1), Status::Ok);
        EXPECT_EQ(xa.set_mark(4097, XArray::Mark::Mark1), Status::Ok);
        EXPECT_TRUE(xa.marked(64, XArray::Mark::Mark1));
        EXPECT_FALSE(xa.marked(3, XArray::Mark::Mark1));

        XArray::Cursor cursor;
        auto found = xa.find_marked(cursor, XArray::Mark::Mark1);
        EXPECT_EQ(found, &b);
        EXPECT_EQ(cursor.index(), 65_usize);

        found = xa.find_marked(cursor, XArray::Mark::Mark1);
        EXPECT_EQ(found, &c);
        EXPECT_EQ(cursor.index(), 4098_usize);

        EXPECT_EQ(xa.clear_mark(64, XArray::Mark::Mark1), Status::Ok);
        EXPECT_FALSE(xa.marked(64, XArray::Mark::Mark1));
    }

    TEST(XArray, FindWalksInIndexOrder) {
        XArray xa;
        Object a {9};
        Object b {1};
        Object c {1000};

        ASSERT_EQ(xa.store(9, &a), Status::Ok);
        ASSERT_EQ(xa.store(1, &b), Status::Ok);
        ASSERT_EQ(xa.store(1000, &c), Status::Ok);

        XArray::Cursor cursor;
        auto found = xa.find(cursor);
        EXPECT_EQ(found, &b);

        found = xa.find(cursor);
        EXPECT_EQ(found, &a);

        found = xa.find(cursor);
        EXPECT_EQ(found, &c);

        EXPECT_EQ(xa.find(cursor), nullptr);
    }
} // namespace
