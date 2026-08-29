// Copyright 2026 The Ours Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <cstddef>

namespace ustl::io {

class BinaryReader {
  public:
    template <typename Table>
    static auto from_payload_of_struct(Table const *table) -> BinaryReader {
        auto const total_size = static_cast<std::size_t>(table->header.size());
        auto const *begin = reinterpret_cast<char const *>(table);
        if (total_size < sizeof(Table)) {
            return {};
        }
        return {begin + sizeof(Table), begin + total_size};
    }

    [[nodiscard]]
    constexpr auto valid() const -> bool {
        return valid_;
    }

    explicit constexpr operator bool() const {
        return valid_ && current_ != end_;
    }

    template <typename Entry>
    auto read() -> Entry const * {
        if (!*this || remaining() < sizeof(Entry)) {
            return nullptr;
        }

        auto const *entry = reinterpret_cast<Entry const *>(current_);
        auto const entry_size = static_cast<std::size_t>(entry->size());
        if (entry_size < sizeof(Entry) || entry_size > remaining()) {
            return nullptr;
        }

        current_ += entry_size;
        return entry;
    }

  private:
    constexpr BinaryReader() = default;

    constexpr BinaryReader(char const *begin, char const *end)
        : current_(begin), end_(end), valid_(true) {}

    [[nodiscard]]
    constexpr auto remaining() const -> std::size_t {
        return static_cast<std::size_t>(end_ - current_);
    }

    char const *current_ = nullptr;
    char const *end_ = nullptr;
    bool valid_ = false;
};

} // namespace ustl::io
