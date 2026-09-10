// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <ustl/io/binary_reader.hpp>

namespace {

struct Entry {
    std::uint8_t type;
    std::uint8_t length;

    auto size() const -> std::uint8_t {
        return length;
    }
};

struct TableHeader {
    std::size_t length;

    auto size() const -> std::size_t {
        return length;
    }
};

struct Table {
    TableHeader header;
    std::uint32_t reserved;
};

struct Fixture {
    Table table;
    Entry first;
    Entry second;
};

} // namespace

auto main() -> int {
    Fixture fixture{
        .table = {.header = {.length = sizeof(Table) + 2 * sizeof(Entry)}, .reserved = 0},
        .first = {.type = 1, .length = sizeof(Entry)},
        .second = {.type = 2, .length = sizeof(Entry)},
    };

    auto reader = ustl::io::BinaryReader::from_payload_of_struct(&fixture.table);
    assert(reader.valid());
    assert(reader);
    assert(reader.read<Entry>() == &fixture.first);
    assert(reader.read<Entry>() == &fixture.second);
    assert(!reader);

    fixture.first.length = 1;
    reader = ustl::io::BinaryReader::from_payload_of_struct(&fixture.table);
    assert(reader.read<Entry>() == nullptr);

    fixture.table.header.length = sizeof(Table) - 1;
    reader = ustl::io::BinaryReader::from_payload_of_struct(&fixture.table);
    assert(!reader.valid());
}
