/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OBI_MEMORY_CONFIG_VIEW_HPP
#define OBI_MEMORY_CONFIG_VIEW_HPP 1

#include <obi/container_view.hpp>
#include <ustl/result.hpp>

#include <limits>

#include <cstring>

namespace obi {
class MemoryConfigView final {
  public:
    using Result = ustl::Result<MemoryConfigView, ours::Status>;
    using RangeResult = ustl::Result<MemoryRange, ours::Status>;

    [[nodiscard]]
    static auto from(ItemView const &item) -> Result {
        if (item.type() != ItemType::Data || item.format() != ItemFormat::MemConfig) {
            return ustl::err(ours::Status::BadType);
        }
        if (item.compression() != Compression::None) {
            return ustl::err(ours::Status::NotSupported);
        }
        if (item.memory_size() != item.payload_size() ||
            item.payload_size() % sizeof(MemoryRange) != 0) {
            return ustl::err(ours::Status::InvalidArgument);
        }
        return ustl::ok(MemoryConfigView(item));
    }

    [[nodiscard]]
    auto count() const -> usize {
        return payload_.size() / sizeof(MemoryRange);
    }

    [[nodiscard]]
    auto range(usize index) const -> RangeResult {
        if (index >= count()) {
            return ustl::err(ours::Status::OutOfRange);
        }
        MemoryRange result{};
        std::memcpy(&result, payload_.data() + index * sizeof(MemoryRange), sizeof(result));
        if (result.reserved != 0 || result.length == 0 ||
            result.base > std::numeric_limits<u64>::max() - result.length ||
            result.type < static_cast<u32>(MemoryRangeType::Ram) ||
            result.type > static_cast<u32>(MemoryRangeType::Reserved)) {
            return ustl::err(ours::Status::InvalidArgument);
        }
        return ustl::ok(result);
    }

  private:
    explicit MemoryConfigView(ItemView item) : payload_(item.payload()) {}

    ItemView::Bytes payload_;
};
} // namespace obi

#endif // OBI_MEMORY_CONFIG_VIEW_HPP
